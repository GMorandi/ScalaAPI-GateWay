#include "forwarder/stream_pipe.h"
#include "protocol/formats.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>
#include <photon/common/timeout.h>

#include <chrono>
#include <cstring>
#include <algorithm>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace gateway::forwarder {

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

StreamPipe::StreamPipe(const StreamPipeConfig& config, ProtocolMode mode,
                       protocol::Format source, protocol::Format target)
    : config_(config), mode_(mode), source_format_(source), target_format_(target) {
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

        // Parse complete SSE events for usage without searching arbitrary text
        // chunks.  This handles a usage object split across reads.
        event_accumulator_.append(read_buf_.data(), n);
        size_t consumed = 0;
        while (true) {
            auto lf = event_accumulator_.find("\n\n", consumed);
            auto crlf = event_accumulator_.find("\r\n\r\n", consumed);
            auto delim = lf;
            size_t delim_size = 2;
            if (crlf != std::string::npos && (delim == std::string::npos || crlf < delim)) {
                delim = crlf;
                delim_size = 4;
            }
            if (delim == std::string::npos) break;
            extract_usage_from_event(
                std::string_view(event_accumulator_.data() + consumed,
                                 delim - consumed + delim_size), result);
            consumed = delim + delim_size;
        }
        if (consumed > 0) event_accumulator_.erase(0, consumed);
        if (event_accumulator_.size() > config_.read_buf_size * 4) {
            event_accumulator_.erase(0, event_accumulator_.size() - config_.read_buf_size);
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

        // Process complete SSE events. Providers use both LF and CRLF, and a
        // delimiter may be split across arbitrary socket reads.
        size_t pos = 0;
        while (true) {
            auto lf_delim = event_accumulator_.find("\n\n", pos);
            auto crlf_delim = event_accumulator_.find("\r\n\r\n", pos);
            auto delim = lf_delim;
            size_t delim_size = 2;
            if (crlf_delim != std::string::npos
                && (delim == std::string::npos || crlf_delim < delim)) {
                delim = crlf_delim;
                delim_size = 4;
            }
            if (delim == std::string::npos) break;

            std::string_view event(event_accumulator_.data() + pos, delim - pos + delim_size);

            // Transform the event between protocols
            auto transformed = transform_event(event);

            // Write transformed event to client
            if (!transformed.empty()) {
                size_t written = 0;
                while (written < transformed.size()) {
                    ssize_t w = write(transformed.data() + written,
                                      transformed.size() - written);
                    if (w <= 0) {
                        result.client_disconnect = true;
                        result.total_duration_ms = static_cast<int>(now_ms() - stream_start);
                        return result;
                    }
                    written += static_cast<size_t>(w);
                }
                result.bytes_forwarded += transformed.size();
            }

            // Extract usage from final events
            if (event.find("\"usage\"") != std::string_view::npos
                || event.find("\"usageMetadata\"") != std::string_view::npos ||
                event.find("message_stop") != std::string_view::npos) {
                extract_usage_from_event(event, result);
            }

            pos = delim + delim_size;
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
    auto data = event_data.find("data:");
    if (data == std::string_view::npos) return;
    data += 5;
    while (data < event_data.size() && (event_data[data] == ' ' || event_data[data] == '\t')) ++data;
    auto end = event_data.find('\n', data);
    if (end == std::string_view::npos) end = event_data.size();
    while (end > data && event_data[end - 1] == '\r') --end;
    if (end <= data || event_data.substr(data, end - data) == "[DONE]") return;

    rapidjson::Document document;
    document.Parse(event_data.data() + data, end - data);
    if (document.HasParseError() || !document.IsObject()) return;
    const rapidjson::Value* usage = nullptr;
    if (document.HasMember("usage") && document["usage"].IsObject()) usage = &document["usage"];
    if (!usage && document.HasMember("usageMetadata") && document["usageMetadata"].IsObject())
        usage = &document["usageMetadata"];
    if (!usage && document.HasMember("response") && document["response"].IsObject()
        && document["response"].HasMember("usage") && document["response"]["usage"].IsObject())
        usage = &document["response"]["usage"];
    if (!usage) return;
    auto integer = [&](const rapidjson::Value& object, const char* key) {
        const auto& value = object[key];
        if (value.IsInt()) return value.GetInt();
        if (value.IsInt64()) return static_cast<int>(value.GetInt64());
        return 0;
    };
    if (usage->HasMember("input_tokens")) result.input_tokens = std::max(result.input_tokens, integer(*usage, "input_tokens"));
    if (usage->HasMember("prompt_tokens")) result.input_tokens = std::max(result.input_tokens, integer(*usage, "prompt_tokens"));
    if (usage->HasMember("promptTokenCount")) result.input_tokens = std::max(result.input_tokens, integer(*usage, "promptTokenCount"));
    if (usage->HasMember("output_tokens")) result.output_tokens = std::max(result.output_tokens, integer(*usage, "output_tokens"));
    if (usage->HasMember("completion_tokens")) result.output_tokens = std::max(result.output_tokens, integer(*usage, "completion_tokens"));
    if (usage->HasMember("candidatesTokenCount")) result.output_tokens = std::max(result.output_tokens, integer(*usage, "candidatesTokenCount"));
    if (usage->HasMember("cache_creation_input_tokens")) result.cache_create_tokens = std::max(result.cache_create_tokens, integer(*usage, "cache_creation_input_tokens"));
    if (usage->HasMember("cache_read_input_tokens")) result.cache_read_tokens = std::max(result.cache_read_tokens, integer(*usage, "cache_read_input_tokens"));
    if (usage->HasMember("cachedContentTokenCount")) result.cache_read_tokens = std::max(result.cache_read_tokens, integer(*usage, "cachedContentTokenCount"));
    if (usage->HasMember("reasoning_tokens")) result.reasoning_tokens = std::max(result.reasoning_tokens, integer(*usage, "reasoning_tokens"));
    if (usage->HasMember("thoughtsTokenCount")) result.reasoning_tokens = std::max(result.reasoning_tokens, integer(*usage, "thoughtsTokenCount"));
    if (usage->HasMember("output_tokens_details") && (*usage)["output_tokens_details"].IsObject()) {
        const auto& details = (*usage)["output_tokens_details"];
        if (details.HasMember("reasoning_tokens"))
            result.reasoning_tokens = std::max(result.reasoning_tokens,
                integer(details, "reasoning_tokens"));
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    usage->Accept(writer);
    result.provider_usage_json.assign(buffer.GetString(),
        std::min<size_t>(buffer.GetSize(), 1024 * 1024));
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
            while (!event_type.empty() && event_type.back() == '\r')
                event_type.remove_suffix(1);
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

    if (mode_ == ProtocolMode::CrossProtocol || source_format_ != target_format_) {
        switch (source_format_) {
        case protocol::Format::Anthropic:
            delta = protocol::anthropic::parse_stream_event(event_type, data_payload);
            break;
        case protocol::Format::OpenAIChatCompletions:
            delta = protocol::openai::parse_stream_event(data_payload);
            break;
        case protocol::Format::OpenAIResponses:
            delta = protocol::openai_responses::parse_stream_event(data_payload);
            break;
        case protocol::Format::Gemini:
            delta = protocol::gemini::parse_stream_event(data_payload);
            break;
        }
        switch (target_format_) {
        case protocol::Format::Anthropic: return protocol::anthropic::serialize_stream_event(delta);
        case protocol::Format::OpenAIChatCompletions: return protocol::openai::serialize_stream_event(delta);
        case protocol::Format::OpenAIResponses: return protocol::openai_responses::serialize_stream_event(delta);
        case protocol::Format::Gemini: return protocol::gemini::serialize_stream_event(delta);
        }
    }

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
