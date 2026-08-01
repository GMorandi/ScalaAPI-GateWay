#include "forwarder/stream_pipe.h"
#include "protocol/formats.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>
#include <photon/common/timeout.h>

#include <chrono>
#include <cstring>

namespace gateway::forwarder {

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

StreamPipe::StreamPipe(const StreamPipeConfig& config, ProtocolMode mode)
    : config_(config), mode_(mode) {
    read_buf_.resize(config_.read_buf_size);
    write_buf_.resize(config_.write_buf_size);
}

StreamResult StreamPipe::run(ReadFn upstream_read, WriteFn client_write) {
    if (mode_ == ProtocolMode::Passthrough) {
        return run_passthrough(upstream_read, client_write);
    }
    return run_transform(upstream_read, client_write);
}

StreamResult StreamPipe::run_passthrough(ReadFn& read, WriteFn& write) {
    StreamResult result;
    auto stream_start = now_ms();
    uint64_t last_data_ms = stream_start;
    uint64_t last_write_ms = stream_start;
    bool first_token_received = false;

    while (true) {
        auto elapsed = now_ms() - stream_start;
        if (elapsed > config_.total_timeout_ms) {
            LOG_WARN("Stream total timeout exceeded ({}ms)", elapsed);
            break;
        }

        // Read from upstream (coroutine yields on epoll until data arrives)
        ssize_t n = read(read_buf_.data(), read_buf_.size());

        if (n < 0) {
            // Timeout or error
            auto since_last = now_ms() - last_data_ms;
            if (!first_token_received && since_last > config_.first_token_timeout_ms) {
                LOG_WARN("First token timeout ({}ms)", since_last);
                break;
            }
            if (first_token_received && since_last > config_.inter_chunk_timeout_ms) {
                LOG_WARN("Inter-chunk timeout ({}ms)", since_last);
                break;
            }
            // Inject keepalive toward client if silent
            if (config_.inject_keepalive) {
                inject_keepalive(write, last_write_ms);
            }
            continue;
        }

        if (n == 0) {
            // Upstream closed connection — stream complete
            result.completed = true;
            break;
        }

        last_data_ms = now_ms();

        if (!first_token_received) {
            first_token_received = true;
            result.first_token_ms = static_cast<int>(last_data_ms - stream_start);
        }

        // Zero-copy: write raw bytes directly to client
        // Coroutine yields if client TCP buffer is full (natural backpressure)
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            ssize_t w = write(read_buf_.data() + written, n - written);
            if (w < 0) {
                // Client disconnected (EPIPE / ECONNRESET)
                result.client_disconnect = true;
                result.total_duration_ms = static_cast<int>(now_ms() - stream_start);
                return result;
            }
            written += w;
            last_write_ms = now_ms();
        }

        result.bytes_forwarded += n;

        // Scan for usage in SSE events (lightweight: look for "usage" keyword)
        std::string_view chunk(read_buf_.data(), n);
        if (chunk.find("\"usage\"") != std::string_view::npos) {
            extract_usage_from_event(chunk, result);
        }
    }

    result.total_duration_ms = static_cast<int>(now_ms() - stream_start);
    return result;
}

StreamResult StreamPipe::run_transform(ReadFn& read, WriteFn& write) {
    StreamResult result;
    auto stream_start = now_ms();
    uint64_t last_data_ms = stream_start;
    bool first_token_received = false;

    while (true) {
        auto elapsed = now_ms() - stream_start;
        if (elapsed > config_.total_timeout_ms) break;

        ssize_t n = read(read_buf_.data(), read_buf_.size());

        if (n < 0) {
            auto since_last = now_ms() - last_data_ms;
            if (!first_token_received && since_last > config_.first_token_timeout_ms) break;
            if (first_token_received && since_last > config_.inter_chunk_timeout_ms) break;
            continue;
        }

        if (n == 0) {
            result.completed = true;
            break;
        }

        last_data_ms = now_ms();

        if (!first_token_received) {
            first_token_received = true;
            result.first_token_ms = static_cast<int>(last_data_ms - stream_start);
        }

        // Accumulate into event buffer and process complete SSE events
        event_accumulator_.append(read_buf_.data(), n);

        // Process complete SSE events (delimited by \n\n)
        size_t pos = 0;
        while (true) {
            auto delim = event_accumulator_.find("\n\n", pos);
            if (delim == std::string::npos) break;

            std::string_view event(event_accumulator_.data() + pos, delim - pos + 2);

            // Transform the event between protocols
            auto transformed = transform_event(event);

            // Write transformed event to client
            if (!transformed.empty()) {
                ssize_t w = write(transformed.data(), transformed.size());
                if (w < 0) {
                    result.client_disconnect = true;
                    result.total_duration_ms = static_cast<int>(now_ms() - stream_start);
                    return result;
                }
                result.bytes_forwarded += transformed.size();
            }

            // Extract usage from final events
            if (event.find("\"usage\"") != std::string_view::npos ||
                event.find("message_stop") != std::string_view::npos) {
                extract_usage_from_event(event, result);
            }

            pos = delim + 2;
        }

        // Keep unprocessed remainder
        if (pos > 0) {
            event_accumulator_.erase(0, pos);
        }
    }

    result.total_duration_ms = static_cast<int>(now_ms() - stream_start);
    return result;
}

void StreamPipe::extract_usage_from_event(std::string_view event_data,
                                           StreamResult& result) {
    // Lightweight extraction: find "input_tokens":N, "output_tokens":N
    // In production: use simdjson for precise parsing
    auto find_int = [&](std::string_view key) -> int {
        auto pos = event_data.find(key);
        if (pos == std::string_view::npos) return 0;
        pos = event_data.find(':', pos + key.size());
        if (pos == std::string_view::npos) return 0;
        ++pos;
        while (pos < event_data.size() && event_data[pos] == ' ') ++pos;
        int val = 0;
        while (pos < event_data.size() && event_data[pos] >= '0' && event_data[pos] <= '9') {
            val = val * 10 + (event_data[pos] - '0');
            ++pos;
        }
        return val;
    };

    if (auto v = find_int("\"input_tokens\""); v > 0) result.input_tokens = v;
    if (auto v = find_int("\"output_tokens\""); v > 0) result.output_tokens = v;
    if (auto v = find_int("\"cache_creation_input_tokens\""); v > 0) result.cache_create_tokens = v;
    if (auto v = find_int("\"cache_read_input_tokens\""); v > 0) result.cache_read_tokens = v;
}

std::string StreamPipe::transform_event(std::string_view event_data) {
    std::string_view event_type;
    std::string_view data_payload;

    // Parse SSE frame: extract "event:" and "data:" fields
    size_t pos = 0;
    while (pos < event_data.size()) {
        auto line_end = event_data.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = event_data.size();
        auto line = event_data.substr(pos, line_end - pos);

        if (line.starts_with("event:")) {
            event_type = line.substr(6);
            while (!event_type.empty() && event_type.front() == ' ')
                event_type.remove_prefix(1);
        } else if (line.starts_with("data:")) {
            auto payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ')
                payload.remove_prefix(1);
            data_payload = payload;
        }
        pos = line_end + 1;
    }

    if (data_payload.empty()) return std::string(event_data);

    protocol::StreamDelta delta;

    switch (mode_) {
    case ProtocolMode::AnthropicToOpenAI:
        delta = protocol::anthropic::parse_stream_event(event_type, data_payload);
        return protocol::openai::serialize_stream_event(delta);

    case ProtocolMode::OpenAIToAnthropic:
        delta = protocol::openai::parse_stream_event(data_payload);
        return protocol::anthropic::serialize_stream_event(delta);

    case ProtocolMode::GeminiCompat:
        delta = protocol::gemini::parse_stream_event(data_payload);
        return protocol::openai::serialize_stream_event(delta);

    default:
        return std::string(event_data);
    }
}

bool StreamPipe::inject_keepalive(WriteFn& write, uint64_t last_write_ms) {
    auto silence = now_ms() - last_write_ms;
    if (silence < config_.keepalive_interval_ms) return false;

    // SSE keepalive: comment line (ignored by SSE clients)
    static constexpr char keepalive[] = ": keepalive\n\n";
    write(keepalive, sizeof(keepalive) - 1);
    return true;
}

}  // namespace gateway::forwarder
