#pragma once

#include "dispatch/capnp_dispatch_client.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gateway::forwarder {

using StreamWriteFn = std::function<ssize_t(const char*, size_t)>;

struct ForwardResult {
    int status_code = 0;
    bool stream = false;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_create_tokens = 0;
    int cache_read_tokens = 0;
    int first_token_ms = 0;
    int duration_ms = 0;
    bool client_disconnect = false;
    std::string error;
};

struct ForwardConfig {
    uint32_t first_token_timeout_ms = 60000;
    uint32_t inter_chunk_timeout_ms = 120000;
    uint32_t total_stream_timeout_ms = 300000;
    uint32_t keepalive_interval_ms = 15000;
    size_t read_buf_size = 64 * 1024;
    size_t write_buf_size = 64 * 1024;
};

class Forwarder {
public:
    static std::unique_ptr<Forwarder> create(const ForwardConfig& config);
    ~Forwarder();

    ForwardResult forward(const dispatch::UpstreamTarget& target,
                          std::string_view body,
                          bool stream,
                          StreamWriteFn stream_write = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::forwarder
