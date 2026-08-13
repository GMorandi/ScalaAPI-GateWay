#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"
#include "platform/metrics.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "dispatch.capnp.h"
#include "types.capnp.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <photon/thread/thread.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace gateway::dispatch {

enum class Method : uint8_t {
    Dispatch = 1,
    ReportUsage = 2,
    Abort = 3,
    ReportUpstreamError = 4,
    MediaOperation = 5,
    RecordLeaseEvidence = 6,
    EvaluateContent = 7,
};

struct CapnpDispatchClient::Impl {
    std::string uds_path;
    int fd = -1;
    photon::mutex mutex;
    uint32_t reconnect_delay_ms = 50;
    std::chrono::steady_clock::time_point reconnect_after{};

    void disconnect() {
        if (fd < 0) return;
        ::close(fd);
        fd = -1;
        reconnect_after = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(reconnect_delay_ms);
        reconnect_delay_ms = std::min<uint32_t>(reconnect_delay_ms * 2, 3000);
    }

    bool ensure_connected();

    bool write_all(const void* data, size_t size) {
        auto* bytes = static_cast<const uint8_t*>(data);
        size_t written = 0;
        while (written < size) {
            auto n = ::send(fd, bytes + written, size - written, MSG_NOSIGNAL);
            if (n <= 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }

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
        if (!write_all(hdr, sizeof(hdr))) return false;
        uint8_t m = static_cast<uint8_t>(method);
        return write_all(&m, 1) && write_all(bytes.begin(), bytes.size());
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

    std::vector<uint8_t> exchange(Method method, kj::ArrayPtr<const capnp::word> words) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensure_connected()) return {};
            if (send_frame(method, words)) {
                auto response = recv_frame();
                if (!response.empty()) return response;
            }
            disconnect();
            if (attempt == 0) photon::thread_usleep(50'000);
        }
        return {};
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
    timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool CapnpDispatchClient::Impl::ensure_connected() {
    if (fd >= 0) {
        char byte;
        auto result = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result > 0 || (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
            return true;
        disconnect();
    }
    if (std::chrono::steady_clock::now() < reconnect_after) return false;
    fd = connect_uds(uds_path);
    if (fd < 0) {
        reconnect_after = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(reconnect_delay_ms);
        reconnect_delay_ms = std::min<uint32_t>(reconnect_delay_ms * 2, 3000);
        return false;
    }
    reconnect_delay_ms = 50;
    platform::global_metrics().dispatch_reconnects.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("Dispatch RPC connected to {}", uds_path);
    return true;
}

std::unique_ptr<CapnpDispatchClient> CapnpDispatchClient::connect(
    const std::string& uds_path) {
    auto client = std::make_unique<CapnpDispatchClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->uds_path = uds_path;

    client->impl_->ensure_connected();
    return client;
}

CapnpDispatchClient::~CapnpDispatchClient() {
    if (impl_ && impl_->fd >= 0) {
        ::close(impl_->fd);
    }
}

DispatchResult CapnpDispatchClient::dispatch(const DispatchRequest& req) {
    DispatchResult result;
    std::lock_guard<photon::mutex> guard(impl_->mutex);

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
    builder.setProtocolVersion(3);
    builder.setStream(req.stream);
    builder.setOperation(req.operation);
    builder.setInboundFormat(req.inbound_format);
    builder.setHttpMethod(req.http_method);
    builder.setRequestPath(req.request_path);
    builder.setContentType(req.content_type);
    builder.setCapability(req.capability);
    builder.setIdempotencyKey(req.idempotency_key);
    builder.setRequestFingerprint(req.request_fingerprint);
    builder.setRealtimeSession(req.realtime_session);
    builder.setForcePlatform(req.force_platform);
    builder.setRequestQuery(req.request_query);
    builder.setRequestBody(req.request_body);

    auto excluded = builder.initExcludedAccounts(req.excluded_accounts.size());
    for (size_t i = 0; i < req.excluded_accounts.size(); ++i) {
        excluded.set(i, req.excluded_accounts[i]);
    }

    auto words = capnp::messageToFlatArray(msg);
    auto resp_bytes = impl_->exchange(Method::Dispatch, words);
    if (resp_bytes.empty()) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "platform unavailable; retry may be safe";
        result.reject_code = kPlatformUnavailableRejectCode;
        return result;
    }

    if (resp_bytes[0] != 0x81 || (resp_bytes.size() - 1) % sizeof(capnp::word) != 0) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "invalid platform dispatch response";
        result.reject_code = kPlatformUnavailableRejectCode;
        return result;
    }
    std::vector<capnp::word> aligned((resp_bytes.size() - 1) / sizeof(capnp::word));
    std::memcpy(aligned.data(), resp_bytes.data() + 1, resp_bytes.size() - 1);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(aligned.data(), aligned.size()));
    auto resp = reader.getRoot<::DispatchResponse>();
    if (resp.getProtocolVersion() != 3) {
        LOG_ERROR("Dispatch protocol version mismatch: expected=3 received={}",
                  resp.getProtocolVersion());
        result.reject_message = "dispatch protocol version mismatch";
        result.reject_code = kPlatformUnavailableRejectCode;
        return result;
    }

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
    if (resp.hasAuth()) result.api_key_id = resp.getAuth().getApiKeyId();

    if (resp.hasReject()) {
        auto reject = resp.getReject();
        result.reject_message = reject.getMessage();
        result.reject_code = static_cast<int>(reject.getCode());
    }
    result.replay_status_code = resp.getReplayStatusCode();
    result.replay_content_type = resp.getReplayContentType();
    result.replay_body = resp.getReplayBody();

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
        result.upstream.http_method = up.getHttpMethod();
        result.upstream.upstream_format = up.getUpstreamFormat();
        result.upstream.websocket_url = up.getWebsocketUrl();
        result.upstream.websocket_protocol = up.getWebsocketProtocol();
        result.upstream.tls_fingerprint_profile_id = up.getTlsFingerprintProfileId();
        result.upstream.media_operation_id = up.getMediaOperationId();
        result.upstream.upstream_task_id = up.getUpstreamTaskId();
        result.upstream.polling_supported = up.getPollingSupported();
        result.upstream.content_download_supported = up.getContentDownloadSupported();

        if (up.hasProxy()) {
            result.upstream.proxy_url = up.getProxy().getUrl();
            result.upstream.proxy_username = up.getProxy().getUsername();
            result.upstream.proxy_password = up.getProxy().getPassword();
        }

        if (up.hasBilling()) {
            result.upstream.rate_multiplier = static_cast<double>(up.getBilling().getRateMultiplier()) / 100000000.0;
            result.upstream.hold_handle = up.getBilling().getHoldHandle();
        }

        auto headers = up.getAuthHeaders();
        for (auto hdr : headers) {
            result.upstream.auth_headers.emplace_back(hdr.getKey(), hdr.getValue());
        }
        for (auto hdr : up.getRequestHeaders()) {
            result.upstream.request_headers.emplace_back(hdr.getKey(), hdr.getValue());
        }
        for (auto name : up.getAllowedResponseHeaders()) {
            result.upstream.allowed_response_headers.emplace_back(name);
        }
        for (auto flag : up.getCapabilityFlags()) {
            result.upstream.capability_flags.emplace_back(flag);
        }
    }

    return result;
}

MediaOperationResult CapnpDispatchClient::media_operation(
    const MediaOperationRequest& req) {
    MediaOperationResult result;
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::MediaOperationRequest>();
    builder.setApiKeyHash(req.api_key_hash);
    builder.setOperationId(req.operation_id);
    builder.setAction(req.action);
    builder.setRequestId(req.request_id);
    builder.setClientIp(req.client_ip);
    builder.setIdempotencyKey(req.idempotency_key);
    builder.setRequestFingerprint(req.request_fingerprint);
    builder.setStatus(req.status);
    builder.setUpstreamTaskId(req.upstream_task_id);
    builder.setOutputMetadata(req.output_metadata);
    builder.setOutputUrl(req.output_url);
    builder.setContentType(req.content_type);
    builder.setProgress(req.progress);

    auto words = capnp::messageToFlatArray(msg);
    auto response = impl_->exchange(Method::MediaOperation, words);
    if (response.empty() || response[0] != 0x85
        || (response.size() - 1) % sizeof(capnp::word) != 0) {
        result.error_code = "platform_unavailable";
        result.error_message = "Platform media operation service unavailable";
        return result;
    }
    std::vector<capnp::word> aligned((response.size() - 1) / sizeof(capnp::word));
    std::memcpy(aligned.data(), response.data() + 1, response.size() - 1);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(aligned.data(), aligned.size()));
    auto wire = reader.getRoot<::MediaOperationResponse>();
    result.accepted = wire.getAccepted();
    result.status_code = wire.getStatusCode();
    result.operation_id = wire.getOperationId();
    result.operation_type = wire.getOperationType();
    result.status = wire.getStatus();
    result.progress = wire.getProgress();
    result.upstream_task_id = wire.getUpstreamTaskId();
    result.output_metadata = wire.getOutputMetadata();
    result.output_url = wire.getOutputUrl();
    result.content_type = wire.getContentType();
    result.error_code = wire.getErrorCode();
    result.error_message = wire.getErrorMessage();
    return result;
}

static RpcAck parse_ack(const std::vector<uint8_t>& response, uint8_t method) {
    RpcAck ack;
    if (response.size() <= 1 || response[0] != method
        || (response.size() - 1) % sizeof(capnp::word) != 0) {
        ack.retryable = true;
        ack.error_code = "invalid_ack";
        return ack;
    }
    std::vector<capnp::word> aligned((response.size() - 1) / sizeof(capnp::word));
    std::memcpy(aligned.data(), response.data() + 1, response.size() - 1);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(aligned.data(), aligned.size()));
    auto wire = reader.getRoot<::WriteAck>();
    ack.accepted = wire.getAccepted();
    ack.duplicate = wire.getDuplicate();
    ack.retryable = wire.getRetryable();
    ack.error_code = wire.getErrorCode();
    return ack;
}

RpcAck CapnpDispatchClient::report_usage(const UsageReportData& report) {
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::UsageReport>();
    builder.setLeaseToken(report.lease_token);
    builder.setInputTokens(report.input_tokens);
    builder.setOutputTokens(report.output_tokens);
    builder.setCacheCreateTokens(report.cache_create_tokens);
    builder.setCacheReadTokens(report.cache_read_tokens);
    builder.setDurationMs(report.duration_ms);
    builder.setFirstTokenMs(report.first_token_ms);
    builder.setStream(report.stream);
    builder.setClientDisconnect(report.client_disconnect);
    builder.setStatusCode(report.status_code);
    builder.setInputImageCount(report.input_image_count);
    builder.setOutputImageCount(report.output_image_count);
    builder.setImageSize(report.image_size);
    builder.setVideoCount(report.video_count);
    builder.setVideoResolution(report.video_resolution);
    builder.setVideoDurationSeconds(report.video_duration_seconds);
    builder.setRealtimeDurationMs(report.realtime_duration_ms);
    builder.setRealtimeFrames(report.realtime_frames);
    builder.setDisconnectReason(report.disconnect_reason);
    builder.setProviderUsageJson(report.provider_usage_json);
    builder.setReasoningTokens(report.reasoning_tokens);
    builder.setServiceTier(report.service_tier);
    builder.setUpstreamEndpoint(report.upstream_endpoint);
    builder.setCancellationReason(report.cancellation_reason);
    builder.setMediaOperationId(report.media_operation_id);
    builder.setPricingVersion(report.pricing_version);
    builder.setResponseStatusCode(report.response_status_code);
    builder.setResponseContentType(report.response_content_type);
    builder.setResponseBody(report.response_body);

    auto words = capnp::messageToFlatArray(msg);
    auto ack = parse_ack(impl_->exchange(Method::ReportUsage, words), 0x82);
    // A missing or malformed frame means the peer closed or corrupted the
    // stream. Drop a live socket so the durable usage reporter reconnects
    // before retrying, without extending backoff for an already closed fd.
    if (ack.error_code == "invalid_ack")
        impl_->disconnect();
    return ack;
}

RpcAck CapnpDispatchClient::record_lease_evidence(
    const std::string& lease_token, LeaseEvidenceStage stage,
    const std::string& detail) {
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::LeaseEvidence>();
    builder.setLeaseToken(lease_token);
    builder.setStage(stage == LeaseEvidenceStage::OutputStarted
        ? ::LeaseEvidence::Stage::OUTPUT_STARTED : ::LeaseEvidence::Stage::FORWARDED);
    builder.setSource("gateway");
    builder.setDetail(detail);

    auto words = capnp::messageToFlatArray(msg);
    return parse_ack(impl_->exchange(Method::RecordLeaseEvidence, words), 0x86);
}

RpcAck CapnpDispatchClient::abort(const std::string& lease_token,
                                  const std::string& reason,
                                  LeaseAbortDisposition disposition,
                                  int provider_status_code) {
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::AbortRequest>();
    builder.setLeaseToken(lease_token);
    builder.setReason(reason);
    builder.setDisposition(disposition == LeaseAbortDisposition::Unknown
        ? ::AbortRequest::Disposition::UNKNOWN : ::AbortRequest::Disposition::NO_CHARGE);
    builder.setProviderStatusCode(provider_status_code);

    auto words = capnp::messageToFlatArray(msg);
    return parse_ack(impl_->exchange(Method::Abort, words), 0x83);
}

RpcAck CapnpDispatchClient::report_upstream_error(const ErrorReportData& error) {
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::ErrorReport>();
    builder.setAccountId(error.account_id);
    builder.setStatusCode(error.status_code);
    builder.setRetryAfterMs(error.retry_after_ms);
    builder.setRequestId(error.request_id);
    builder.setErrorMessage(error.error_message);

    auto words = capnp::messageToFlatArray(msg);
    return parse_ack(impl_->exchange(Method::ReportUpstreamError, words), 0x84);
}

ContentPolicyResult CapnpDispatchClient::evaluate_response_content(
    const std::string& lease_token, const std::string& content,
    const std::string& capability) {
    ContentPolicyResult result;
    std::lock_guard<photon::mutex> guard(impl_->mutex);

    capnp::MallocMessageBuilder msg;
    auto builder = msg.initRoot<::ContentPolicyRequest>();
    builder.setLeaseToken(lease_token);
    builder.setContent(content);
    builder.setCapability(capability);
    builder.setStage(::ContentPolicyRequest::Stage::RESPONSE);

    auto words = capnp::messageToFlatArray(msg);
    auto response = impl_->exchange(Method::EvaluateContent, words);
    if (response.size() <= 1 || response[0] != 0x87
        || (response.size() - 1) % sizeof(capnp::word) != 0) {
        impl_->disconnect();
        return result;
    }

    std::vector<capnp::word> aligned((response.size() - 1) / sizeof(capnp::word));
    std::memcpy(aligned.data(), response.data() + 1, response.size() - 1);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(aligned.data(), aligned.size()));
    auto wire = reader.getRoot<::ContentPolicyResponse>();
    result.evaluated = wire.getEvaluated();
    result.allowed = wire.getAllowed();
    result.retryable = wire.getRetryable();
    result.error_code = wire.getErrorCode();
    result.matched_rule_id = wire.getMatchedRuleId();
    result.message = wire.getMessage();
    return result;
}

bool CapnpDispatchClient::is_connected() {
    std::lock_guard<photon::mutex> guard(impl_->mutex);
    return impl_->ensure_connected();
}

}  // namespace gateway::dispatch
