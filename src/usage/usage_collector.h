#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gateway::usage {

struct UsageEvent {
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

class UsageCollector {
public:
    explicit UsageCollector(std::string database_path = {});
    ~UsageCollector();

    void record(UsageEvent event);
    std::vector<UsageEvent> peek(size_t limit = 100);
    void acknowledge(const std::string& lease_token);
    void dead_letter(const std::string& lease_token, const std::string& error_code);
    std::vector<UsageEvent> drain();
    size_t pending() const;
    size_t dead_lettered() const;
    bool durable() const;

    struct Evidence {
        std::string lease_token;
        std::string stage;
        std::string source;
        std::string detail;
    };

    void record_evidence(std::string lease_token, std::string stage,
                         std::string source, std::string detail);
    std::vector<Evidence> peek_evidence(size_t limit = 100);
    void acknowledge_evidence(const std::string& lease_token, const std::string& stage);
    void dead_letter_evidence(const std::string& lease_token, const std::string& stage,
                              const std::string& error_code);
    size_t pending_evidence() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::usage
