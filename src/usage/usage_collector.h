#pragma once

#include "dispatch/capnp_dispatch_client.h"
#include <string>
#include <vector>
#include <cstdint>

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
};

class UsageCollector {
public:
    void record(UsageEvent event);
    std::vector<UsageEvent> drain();
    size_t pending() const;

private:
    static constexpr size_t kRingBufferSize = 4096;
    UsageEvent ring_buffer_[kRingBufferSize];
    size_t write_pos_ = 0;
    size_t count_ = 0;
};

}  // namespace gateway::usage
