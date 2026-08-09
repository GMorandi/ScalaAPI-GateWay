#pragma once

#include <photon/net/socket.h>
#include "protocol/converter.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace gateway::forwarder {

enum class StreamPolicyDisposition {
    Allow,
    Block,
    FailClosed,
};

struct StreamPolicyDecision {
    StreamPolicyDisposition disposition = StreamPolicyDisposition::FailClosed;
    std::string error_code;
    std::string message;

    static StreamPolicyDecision Allowed() {
        return {StreamPolicyDisposition::Allow, {}, {}};
    }

    static StreamPolicyDecision Blocked(std::string code, std::string message) {
        return {StreamPolicyDisposition::Block, std::move(code), std::move(message)};
    }

    static StreamPolicyDecision FailedClosed(std::string code, std::string message) {
        return {StreamPolicyDisposition::FailClosed, std::move(code), std::move(message)};
    }
};

using StreamPolicyFn = std::function<StreamPolicyDecision(std::string_view)>;

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
    bool policy_blocked = false;
    bool policy_failed_closed = false;
    std::string policy_error_code;
    std::string policy_message;
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
    size_t max_policy_event_bytes = 128 * 1024;
    bool inject_keepalive = true;
    StreamPolicyFn policy;
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
    bool apply_policy(std::string_view provider_event, std::string_view client_event,
                      StreamResult& result);
    bool write_all(WriteFn& write, std::string_view data, StreamResult& result);
    std::string policy_error_event(const StreamResult& result) const;
    bool emit_policy_error(WriteFn& write, StreamResult& result);
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
