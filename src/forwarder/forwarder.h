#pragma once

#include "dispatch/capnp_dispatch_client.h"
#include "forwarder/stream_pipe.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gateway::forwarder {

using StreamWriteFn = std::function<ssize_t(const char*, size_t)>;
using ResponseStartFn = std::function<void(
    int, std::string_view,
    const std::vector<std::pair<std::string, std::string>>&)>;

struct ForwardRequest {
    std::string_view method;
    std::string_view body;
    std::string_view content_type;
    std::string_view accept;
    std::string_view user_agent;
    std::string_view request_id;
    std::string_view idempotency_key;
    std::vector<std::pair<std::string, std::string>> headers;
    bool stream = false;
    protocol::Format stream_source = protocol::Format::OpenAIChatCompletions;
    protocol::Format stream_target = protocol::Format::Anthropic;
    StreamWriteFn stream_write;
    ResponseStartFn response_start;
};

struct ForwardResult {
    int status_code = 0;
    bool stream = false;
    std::string body;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_create_tokens = 0;
    int cache_read_tokens = 0;
    int first_token_ms = 0;
    int duration_ms = 0;
    bool client_disconnect = false;
    bool output_started = false;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> response_headers;
    int retry_after_ms = 0;
    int reasoning_tokens = 0;
    std::string provider_usage_json;
    std::string service_tier;
    std::string error;
};

struct ForwardConfig {
    uint32_t first_token_timeout_ms = 60000;
    uint32_t inter_chunk_timeout_ms = 120000;
    uint32_t total_stream_timeout_ms = 300000;
    uint32_t keepalive_interval_ms = 15000;
    size_t read_buf_size = 64 * 1024;
    size_t write_buf_size = 64 * 1024;
    size_t max_response_body_size = 64 * 1024 * 1024;
};

class Forwarder {
public:
    static std::unique_ptr<Forwarder> create(const ForwardConfig& config);
    ~Forwarder();

    ForwardResult forward(const dispatch::UpstreamTarget& target,
                          const ForwardRequest& request,
                          ProtocolMode protocol_mode = ProtocolMode::Passthrough);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::forwarder
