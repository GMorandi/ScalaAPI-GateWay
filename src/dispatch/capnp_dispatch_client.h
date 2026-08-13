#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace gateway::dispatch {

struct DispatchRequest {
    enum class EndpointKind {
        Messages = 0,
        ChatCompletions = 1,
        Responses = 2,
        Embeddings = 3,
        Images = 4,
        Gemini = 5,
        Videos = 6,
        CountTokens = 7,
        Models = 8,
        AlphaSearch = 9,
        Realtime = 10,
        Antigravity = 11,
    };

    std::string api_key_hash;
    std::string requested_model;
    std::string session_hash;
    std::string client_ip;
    std::string request_id;
    std::vector<int64_t> excluded_accounts;
    int64_t cached_auth_version = 0;
    int endpoint = 0;
    std::string metadata_user_id;
    bool stream = false;
    std::string operation;
    std::string inbound_format;
    std::string http_method;
    std::string request_path;
    std::string content_type;
    std::string capability;
    std::string idempotency_key;
    bool realtime_session = false;
    std::string force_platform;
    std::string request_fingerprint;
    std::string request_query;
    std::string request_body;
};

struct MediaOperationRequest {
    std::string api_key_hash;
    std::string operation_id;
    std::string action;
    std::string request_id;
    std::string client_ip;
    std::string idempotency_key;
    std::string request_fingerprint;
    std::string status;
    std::string upstream_task_id;
    std::string output_metadata;
    std::string output_url;
    std::string content_type;
    int progress = 0;
};

struct UpstreamTarget {
    int64_t account_id = 0;
    std::string platform;
    std::string base_url;
    std::string upstream_path;
    std::vector<std::pair<std::string, std::string>> auth_headers;
    std::string mapped_model;
    std::string proxy_url;
    std::string proxy_username;
    std::string proxy_password;
    int64_t user_id = 0;
    int64_t group_id = 0;
    double rate_multiplier = 1.0;
    std::string hold_handle;
    bool tls_fingerprint = false;
    std::string http_method;
    std::string upstream_format;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::vector<std::string> allowed_response_headers;
    std::string websocket_url;
    std::string websocket_protocol;
    std::string tls_fingerprint_profile_id;
    std::vector<std::string> capability_flags;
    std::string media_operation_id;
    std::string upstream_task_id;
    bool polling_supported = false;
    bool content_download_supported = false;
};

struct DispatchResult {
    enum class Outcome { Ok, Wait, Rejected, Reauth };
    Outcome outcome = Outcome::Rejected;
    int64_t auth_version = 0;
    int64_t api_key_id = 0;
    std::string lease_token;
    UpstreamTarget upstream;
    std::string reject_message;
    int reject_code = 0;
    int wait_timeout_ms = 0;
    int replay_status_code = 0;
    std::string replay_content_type;
    std::string replay_body;
};

inline constexpr int kPlatformUnavailableRejectCode = 12;

inline bool is_retryable_platform_dispatch(const DispatchResult& result) {
    return result.outcome == DispatchResult::Outcome::Rejected
        && result.reject_code == kPlatformUnavailableRejectCode;
}

inline int platform_dispatch_retry_delay_ms(int retry_number) {
    if (retry_number <= 0) return 50;
    return std::min(1000, 50 * (1 << std::min(retry_number - 1, 5)));
}

struct MediaOperationResult {
    bool accepted = false;
    int status_code = 500;
    std::string operation_id;
    std::string operation_type;
    std::string status;
    int progress = 0;
    std::string upstream_task_id;
    std::string output_metadata;
    std::string output_url;
    std::string content_type;
    std::string error_code;
    std::string error_message;
};

struct UsageReportData {
    std::string lease_token;
    std::string request_id;
    int64_t api_key_id = 0;
    int64_t user_id = 0;
    int64_t account_id = 0;
    int64_t group_id = 0;
    std::string model;
    std::string upstream_model;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_create_tokens = 0;
    int cache_read_tokens = 0;
    int duration_ms = 0;
    int first_token_ms = 0;
    bool stream = false;
    bool client_disconnect = false;
    int status_code = 0;
    int input_image_count = 0;
    int output_image_count = 0;
    std::string image_size;
    int video_count = 0;
    std::string video_resolution;
    int video_duration_seconds = 0;
    int realtime_duration_ms = 0;
    int realtime_frames = 0;
    std::string disconnect_reason;
    std::string provider_usage_json;
    int reasoning_tokens = 0;
    std::string service_tier;
    std::string upstream_endpoint;
    std::string cancellation_reason;
    std::string media_operation_id;
    std::string pricing_version;
    int response_status_code = 0;
    std::string response_content_type;
    std::string response_body;
};

struct RpcAck {
    bool accepted = false;
    bool duplicate = false;
    bool retryable = false;
    std::string error_code;

    bool acknowledged() const { return accepted || duplicate; }
};

struct ContentPolicyResult {
    bool evaluated = false;
    bool allowed = false;
    bool retryable = false;
    std::string error_code = "platform_unavailable";
    int64_t matched_rule_id = 0;
    std::string message;
};

enum class ContentPolicyDisposition {
    Allow,
    Block,
    FailClosed,
};

inline ContentPolicyDisposition content_policy_disposition(
    const ContentPolicyResult& result) {
    if (!result.evaluated || result.retryable)
        return ContentPolicyDisposition::FailClosed;
    return result.allowed
        ? ContentPolicyDisposition::Allow : ContentPolicyDisposition::Block;
}

enum class LeaseEvidenceStage {
    Forwarded,
    OutputStarted,
};

enum class LeaseAbortDisposition {
    NoCharge,
    Unknown,
};

struct ErrorReportData {
    int64_t account_id = 0;
    int status_code = 0;
    int retry_after_ms = 0;
    std::string request_id;
    std::string error_message;
};

class CapnpDispatchClient {
public:
    static std::unique_ptr<CapnpDispatchClient> connect(const std::string& uds_path);
    ~CapnpDispatchClient();

    DispatchResult dispatch(const DispatchRequest& req);
    MediaOperationResult media_operation(const MediaOperationRequest& req);
    RpcAck report_usage(const UsageReportData& report);
    RpcAck record_lease_evidence(const std::string& lease_token,
                                 LeaseEvidenceStage stage,
                                 const std::string& detail = "");
    RpcAck abort(const std::string& lease_token, const std::string& reason,
                 LeaseAbortDisposition disposition = LeaseAbortDisposition::NoCharge,
                 int provider_status_code = 0);
    RpcAck report_upstream_error(const ErrorReportData& error);
    ContentPolicyResult evaluate_response_content(
        const std::string& lease_token, const std::string& content,
        const std::string& capability);
    bool is_connected();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::dispatch
