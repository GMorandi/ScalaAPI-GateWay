#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <span>

namespace gateway::dispatch {

struct CapnpDispatchClient::Impl {
    std::string uds_path;
    int fd = -1;
    char read_buf[256 * 1024];

    bool send_frame(std::string_view payload) {
        if (fd < 0) return false;
        uint32_t len = static_cast<uint32_t>(payload.size());
        uint8_t hdr[4] = {
            static_cast<uint8_t>(len & 0xFF),
            static_cast<uint8_t>((len >> 8) & 0xFF),
            static_cast<uint8_t>((len >> 16) & 0xFF),
            static_cast<uint8_t>((len >> 24) & 0xFF),
        };
        if (::write(fd, hdr, 4) != 4) return false;
        size_t total = 0;
        while (total < payload.size()) {
            ssize_t n = ::write(fd, payload.data() + total, payload.size() - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    std::string recv_frame() {
        if (fd < 0) return "";
        uint8_t hdr[4];
        size_t got = 0;
        while (got < 4) {
            ssize_t n = ::read(fd, hdr + got, 4 - got);
            if (n <= 0) return "";
            got += n;
        }
        uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        if (len == 0 || len > sizeof(read_buf)) return "";

        std::string result;
        result.resize(len);
        got = 0;
        while (got < len) {
            ssize_t n = ::read(fd, result.data() + got, len - got);
            if (n <= 0) return "";
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

static std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string extract_json_string(std::string_view json, std::string_view key) {
    auto pos = json.find(std::format("\"{}\"", key));
    if (pos == std::string_view::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string_view::npos) return "";
    auto end = json.find('"', pos + 1);
    while (end != std::string_view::npos && json[end - 1] == '\\') {
        end = json.find('"', end + 1);
    }
    if (end == std::string_view::npos) return "";
    return std::string(json.substr(pos + 1, end - pos - 1));
}

static int64_t extract_json_int(std::string_view json, std::string_view key) {
    auto pos = json.find(std::format("\"{}\"", key));
    if (pos == std::string_view::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    bool neg = false;
    if (pos < json.size() && json[pos] == '-') { neg = true; pos++; }
    int64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return neg ? -val : val;
}

static double extract_json_double(std::string_view json, std::string_view key) {
    auto pos = json.find(std::format("\"{}\"", key));
    if (pos == std::string_view::npos) return 0.0;
    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return 0.0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return std::strtod(std::string(json.substr(pos, 32)).c_str(), nullptr);
}

static bool extract_json_bool(std::string_view json, std::string_view key) {
    auto pos = json.find(std::format("\"{}\"", key));
    if (pos == std::string_view::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return false;
    return json.find("true", pos) < json.find('\n', pos);
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
    LOG_INFO("Dispatch RPC connected to {}", uds_path);
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

    std::string excluded;
    for (size_t i = 0; i < req.excluded_accounts.size(); ++i) {
        if (i > 0) excluded += ",";
        excluded += std::to_string(req.excluded_accounts[i]);
    }

    auto msg = std::format(
        R"({{"method":"dispatch","apiKeyHash":"{}","requestedModel":"{}","sessionHash":"{}","clientIp":"{}","requestId":"{}","excludedAccounts":[{}],"cachedAuthVersion":{},"endpoint":{},"metadataUserId":"{}"}})",
        json_escape(req.api_key_hash),
        json_escape(req.requested_model),
        json_escape(req.session_hash),
        json_escape(req.client_ip),
        json_escape(req.request_id),
        excluded,
        req.cached_auth_version,
        req.endpoint,
        json_escape(req.metadata_user_id));

    if (!impl_->send_frame(msg)) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "send failed";
        return result;
    }

    auto resp = impl_->recv_frame();
    if (resp.empty()) {
        result.outcome = DispatchResult::Outcome::Rejected;
        result.reject_message = "no response";
        return result;
    }

    auto outcome = extract_json_string(resp, "outcome");
    if (outcome == "ok") result.outcome = DispatchResult::Outcome::Ok;
    else if (outcome == "wait") result.outcome = DispatchResult::Outcome::Wait;
    else if (outcome == "reauth") result.outcome = DispatchResult::Outcome::Reauth;
    else result.outcome = DispatchResult::Outcome::Rejected;

    result.auth_version = extract_json_int(resp, "authVersion");
    result.lease_token = extract_json_string(resp, "leaseToken");
    result.reject_message = extract_json_string(resp, "rejectMessage");
    result.reject_code = static_cast<int>(extract_json_int(resp, "rejectCode"));
    result.wait_timeout_ms = static_cast<int>(extract_json_int(resp, "waitTimeoutMs"));

    result.upstream.account_id = extract_json_int(resp, "accountId");
    result.upstream.platform = extract_json_string(resp, "platform");
    result.upstream.base_url = extract_json_string(resp, "baseUrl");
    result.upstream.upstream_path = extract_json_string(resp, "upstreamPath");
    result.upstream.mapped_model = extract_json_string(resp, "mappedModel");
    result.upstream.proxy_url = extract_json_string(resp, "proxyUrl");
    result.upstream.user_id = extract_json_int(resp, "userId");
    result.upstream.group_id = extract_json_int(resp, "groupId");
    result.upstream.rate_multiplier = extract_json_double(resp, "rateMultiplier");
    result.upstream.hold_handle = extract_json_string(resp, "holdHandle");
    result.upstream.tls_fingerprint = extract_json_bool(resp, "tlsFingerprint");

    auto auth_hdrs_pos = resp.find("\"authHeaders\"");
    if (auth_hdrs_pos != std::string::npos) {
        auto arr_start = resp.find('[', auth_hdrs_pos);
        auto arr_end = resp.find(']', arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            auto arr = std::string_view(resp).substr(arr_start, arr_end - arr_start + 1);
            size_t pos = 0;
            while (true) {
                auto key_pos = arr.find("\"key\"", pos);
                if (key_pos == std::string::npos) break;
                auto val_pos = arr.find("\"value\"", key_pos);
                if (val_pos == std::string::npos) break;
                auto k = extract_json_string(arr.substr(key_pos), "key");
                auto v = extract_json_string(arr.substr(val_pos), "value");
                result.upstream.auth_headers.emplace_back(k, v);
                pos = val_pos + 1;
            }
        }
    }

    return result;
}

void CapnpDispatchClient::report_usage(const UsageReportData& report) {
    if (impl_->fd < 0) return;

    auto msg = std::format(
        R"({{"method":"reportUsage","leaseToken":"{}","requestId":"{}","apiKeyId":{},"userId":{},"accountId":{},"groupId":{},"model":"{}","upstreamModel":"{}","inputTokens":{},"outputTokens":{},"cacheCreateTokens":{},"cacheReadTokens":{},"durationMs":{},"firstTokenMs":{},"stream":{},"clientDisconnect":{}}})",
        json_escape(report.lease_token),
        json_escape(report.request_id),
        report.api_key_id, report.user_id, report.account_id, report.group_id,
        json_escape(report.model), json_escape(report.upstream_model),
        report.input_tokens, report.output_tokens,
        report.cache_create_tokens, report.cache_read_tokens,
        report.duration_ms, report.first_token_ms,
        report.stream ? "true" : "false",
        report.client_disconnect ? "true" : "false");

    impl_->send_frame(msg);
    impl_->recv_frame();
}

void CapnpDispatchClient::abort(const std::string& lease_token,
                                 const std::string& reason) {
    if (impl_->fd < 0) return;

    auto msg = std::format(
        R"({{"method":"abort","leaseToken":"{}","reason":"{}"}})",
        json_escape(lease_token), json_escape(reason));

    impl_->send_frame(msg);
    impl_->recv_frame();
}

void CapnpDispatchClient::report_upstream_error(const ErrorReportData& error) {
    if (impl_->fd < 0) return;

    auto msg = std::format(
        R"({{"method":"reportUpstreamError","accountId":{},"statusCode":{},"retryAfterMs":{},"requestId":"{}","errorMessage":"{}"}})",
        error.account_id, error.status_code, error.retry_after_ms,
        json_escape(error.request_id), json_escape(error.error_message));

    impl_->send_frame(msg);
    impl_->recv_frame();
}

}  // namespace gateway::dispatch
