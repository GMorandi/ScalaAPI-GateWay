#pragma once

#include <photon/net/socket.h>
#include "protocol/converter.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

namespace gateway::forwarder {

struct StreamResult {
    int64_t bytes_forwarded = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_create_tokens = 0;
    int cache_read_tokens = 0;
    int reasoning_tokens = 0;
    int first_token_ms = 0;
    int total_duration_ms = 0;
    bool completed = false;
    bool terminal_event_seen = false;
    bool incomplete = false;
    bool provider_disconnect = false;
    bool timed_out = false;
    bool client_disconnect = false;
    bool malformed_usage = false;
    std::string provider_usage_json;
};

enum class ProtocolMode {
    Passthrough,
    AnthropicToOpenAI,
    OpenAIToAnthropic,
    GeminiCompat,
    CrossProtocol,
};

struct StreamPipeConfig {
    size_t read_buf_size = 64 * 1024;
    size_t write_buf_size = 64 * 1024;
    uint32_t first_token_timeout_ms = 60'000;
    uint32_t inter_chunk_timeout_ms = 120'000;
    uint32_t total_timeout_ms = 300'000;
    uint32_t keepalive_interval_ms = 15'000;
    bool inject_keepalive = true;
};

using WriteFn = std::function<ssize_t(const char*, size_t)>;
using ReadFn = std::function<ssize_t(char*, size_t)>;

class StreamPipe {
public:
    explicit StreamPipe(const StreamPipeConfig& config, ProtocolMode mode,
                        protocol::Format source = protocol::Format::OpenAIChatCompletions,
                        protocol::Format target = protocol::Format::Anthropic);

    StreamResult run(ReadFn upstream_read, WriteFn client_write);

private:
    StreamResult run_passthrough(ReadFn& read, WriteFn& write);
    StreamResult run_transform(ReadFn& read, WriteFn& write);

    void extract_usage_from_event(std::string_view event_data, StreamResult& result);
    bool is_terminal_event(std::string_view event_data) const;
    std::string transform_event(std::string_view event_data);
    bool inject_keepalive(WriteFn& write, uint64_t last_write_ms);

    StreamPipeConfig config_;
    ProtocolMode mode_;
    protocol::Format source_format_;
    protocol::Format target_format_;
    std::string read_buf_;
    std::string write_buf_;
    std::string event_accumulator_;
};

}  // namespace gateway::forwarder
