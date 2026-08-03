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
};

class UsageCollector {
public:
    explicit UsageCollector(std::string database_path = {});
    ~UsageCollector();

    void record(UsageEvent event);
    std::vector<UsageEvent> peek(size_t limit = 100);
    void acknowledge(const std::string& lease_token);
    std::vector<UsageEvent> drain();
    size_t pending() const;
    bool durable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::usage
