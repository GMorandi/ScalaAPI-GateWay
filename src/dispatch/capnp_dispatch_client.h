#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace gateway::dispatch {

struct DispatchRequest {
    enum class EndpointKind {
        Messages = 0,
        ChatCompletions = 1,
        Responses = 2,
        Embeddings = 3,
        Images = 4,
        Gemini = 5,
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
};

struct UpstreamTarget {
    int64_t account_id = 0;
    std::string platform;
    std::string base_url;
    std::string upstream_path;
    std::vector<std::pair<std::string, std::string>> auth_headers;
    std::string mapped_model;
    std::string proxy_url;
    int64_t user_id = 0;
    int64_t group_id = 0;
    double rate_multiplier = 1.0;
    std::string hold_handle;
    bool tls_fingerprint = false;
};

struct DispatchResult {
    enum class Outcome { Ok, Wait, Rejected, Reauth };
    Outcome outcome = Outcome::Rejected;
    int64_t auth_version = 0;
    std::string lease_token;
    UpstreamTarget upstream;
    std::string reject_message;
    int reject_code = 0;
    int wait_timeout_ms = 0;
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
    void report_usage(const UsageReportData& report);
    void abort(const std::string& lease_token, const std::string& reason);
    void report_upstream_error(const ErrorReportData& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::dispatch
