#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "dispatch.capnp.h"
#include "types.capnp.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

namespace gateway::dispatch {

enum class Method : uint8_t {
    Dispatch = 1,
    ReportUsage = 2,
    Abort = 3,
    ReportUpstreamError = 4,
};

struct CapnpDispatchClient::Impl {
    std::string uds_path;
    int fd = -1;

    bool send_frame(Method method, kj::ArrayPtr<const capnp::word> words) {
        if (fd < 0) return false;
        auto bytes = words.asBytes();
        uint32_t len = static_cast<uint32_t>(bytes.size() + 1);
        uint8_t hdr[4] = {
            static_cast<uint8_t>(len & 0xFF),
            static_cast<uint8_t>((len >> 8) & 0xFF),
            static_cast<uint8_t>((len >> 16) & 0xFF),
            static_cast<uint8_t>((len >> 24) & 0xFF),
        };
        if (::write(fd, hdr, 4) != 4) return false;
        uint8_t m = static_cast<uint8_t>(method);
        if (::write(fd, &m, 1) != 1) return false;
        size_t total = 0;
        while (total < bytes.size()) {
            ssize_t n = ::write(fd, bytes.begin() + total, bytes.size() - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    std::vector<uint8_t> recv_frame() {
        if (fd < 0) return {};
        uint8_t hdr[4];
        size_t got = 0;
        while (got < 4) {
            ssize_t n = ::read(fd, hdr + got, 4 - got);
            if (n <= 0) return {};
            got += n;
        }
        uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        if (len == 0 || len > 1024 * 1024) return {};

        std::vector<uint8_t> result(len);
        got = 0;
        while (got < len) {
            ssize_t n = ::read(fd, result.data() + got, len - got);
            if (n <= 0) return {};
            got += n;
        }
        return result;
    }
};

static int connect_uds(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::unique_ptr<CapnpDispatchClient> CapnpDispatchClient::connect(
    const std::string& uds_path) {
    auto client = std::make_unique<CapnpDispatchClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->uds_path = uds_path;

    int fd = connect_uds(uds_path);
    if (fd < 0) {
        LOG_ERROR("Dispatch RPC: cannot connect to {}", uds_path);
        return client;
    }
    client->impl_->fd = fd;
    LOG_INFO("Dispatch RPC connected to {} (capnp binary)", uds_path);
    return client;
}

CapnpDispatchClient::~CapnpDispatchClient() {
    if (impl_ && impl_->fd >= 0) {
        ::close(impl_->fd);
    }
}

DispatchResult CapnpDispatchClient::dispatch(const DispatchRequest& req) {
    DispatchResult result;

    if (impl_->fd < 0) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "RPC not connected";
        return result;
    }

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::DispatchRequest>();
    builder.setApiKeyHash(req.api_key_hash);
    builder.setRequestedModel(req.requested_model);
    builder.setSessionHash(req.session_hash);
    builder.setClientIp(req.client_ip);
    builder.setRequestId(req.request_id);
    builder.setCachedAuthVersion(req.cached_auth_version);
    builder.setEndpoint(static_cast<::DispatchRequest::EndpointKind>(req.endpoint));
    builder.setMetadataUserId(req.metadata_user_id);

    auto excluded = builder.initExcludedAccounts(req.excluded_accounts.size());
    for (size_t i = 0; i < req.excluded_accounts.size(); ++i) {
        excluded.set(i, req.excluded_accounts[i]);
    }

    auto words = capnp::messageToFlatArray(msg);
    if (!impl_->send_frame(Method::Dispatch, words)) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "send failed";
        return result;
    }

    auto resp_bytes = impl_->recv_frame();
    if (resp_bytes.empty()) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "no response";
        return result;
    }

    // Skip 1-byte method prefix in response
    auto payload = kj::arrayPtr(
        reinterpret_cast<const capnp::word*>(resp_bytes.data() + 1),
        (resp_bytes.size() - 1) / sizeof(capnp::word));
    capnp::FlatArrayMessageReader reader(payload);
    auto resp = reader.getRoot<::DispatchResponse>();

    switch (resp.getOutcome()) {
        case ::DispatchResponse::Outcome::OK:
            result.outcome = DispatchResult::Outcome::Ok; break;
        case ::DispatchResponse::Outcome::WAIT:
            result.outcome = DispatchResult::Outcome::Wait; break;
        case ::DispatchResponse::Outcome::REAUTH:
            result.outcome = DispatchResult::Outcome::Reauth; break;
        default:
            result.outcome = DispatchResult::Outcome::Rejected; break;
    }

    result.auth_version = resp.getAuthVersion();
    result.lease_token = resp.getLeaseToken();

    if (resp.hasReject()) {
        auto reject = resp.getReject();
        result.reject_message = reject.getMessage();
        result.reject_code = static_cast<int>(reject.getCode());
    }

    if (resp.hasWaitPlan()) {
        result.wait_timeout_ms = resp.getWaitPlan().getTimeoutMs();
    }

    if (resp.hasUpstream()) {
        auto up = resp.getUpstream();
        result.upstream.account_id = up.getAccountId();
        result.upstream.platform = up.getPlatform();
        result.upstream.base_url = up.getBaseUrl();
        result.upstream.upstream_path = up.getUpstreamPath();
        result.upstream.mapped_model = up.getMappedModel();
        result.upstream.user_id = up.getUserId();
        result.upstream.group_id = up.getGroupId();
        result.upstream.tls_fingerprint = up.getTlsFingerprint();

        if (up.hasProxy()) {
            result.upstream.proxy_url = up.getProxy().getUrl();
        }

        if (up.hasBilling()) {
            result.upstream.rate_multiplier = up.getBilling().getRateMultiplier();
            result.upstream.hold_handle = up.getBilling().getHoldHandle();
        }

        auto headers = up.getAuthHeaders();
        for (auto hdr : headers) {
            result.upstream.auth_headers.emplace_back(hdr.getKey(), hdr.getValue());
        }
    }

    return result;
}

void CapnpDispatchClient::report_usage(const UsageReportData& report) {
    if (impl_->fd < 0) return;

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::UsageReport>();
    builder.setLeaseToken(report.lease_token);
    builder.setRequestId(report.request_id);
    builder.setApiKeyId(report.api_key_id);
    builder.setUserId(report.user_id);
    builder.setAccountId(report.account_id);
    builder.setGroupId(report.group_id);
    builder.setModel(report.model);
    builder.setUpstreamModel(report.upstream_model);
    builder.setInputTokens(report.input_tokens);
    builder.setOutputTokens(report.output_tokens);
    builder.setCacheCreateTokens(report.cache_create_tokens);
    builder.setCacheReadTokens(report.cache_read_tokens);
    builder.setDurationMs(report.duration_ms);
    builder.setFirstTokenMs(report.first_token_ms);
    builder.setStream(report.stream);
    builder.setClientDisconnect(report.client_disconnect);

    auto words = capnp::messageToFlatArray(msg);
    impl_->send_frame(Method::ReportUsage, words);
    impl_->recv_frame();
}

void CapnpDispatchClient::abort(const std::string& lease_token,
                                 const std::string& reason) {
    if (impl_->fd < 0) return;

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::AbortRequest>();
    builder.setLeaseToken(lease_token);
    builder.setReason(reason);

    auto words = capnp::messageToFlatArray(msg);
    impl_->send_frame(Method::Abort, words);
    impl_->recv_frame();
}

void CapnpDispatchClient::report_upstream_error(const ErrorReportData& error) {
    if (impl_->fd < 0) return;

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::ErrorReport>();
    builder.setAccountId(error.account_id);
    builder.setStatusCode(error.status_code);
    builder.setRetryAfterMs(error.retry_after_ms);
    builder.setRequestId(error.request_id);
    builder.setErrorMessage(error.error_message);

    auto words = capnp::messageToFlatArray(msg);
    impl_->send_frame(Method::ReportUpstreamError, words);
    impl_->recv_frame();
}

}  // namespace gateway::dispatch
