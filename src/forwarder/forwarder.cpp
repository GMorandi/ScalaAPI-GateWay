#include "forwarder/forwarder.h"
#include "forwarder/connection_pool.h"
#include "forwarder/stream_pipe.h"
#include "platform/logging.h"

#include <photon/net/http/client.h>
#include <photon/net/http/message.h>
#include <photon/thread/thread.h>

#include <chrono>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace gateway::forwarder {

struct Forwarder::Impl {
    ForwardConfig config;
    std::unique_ptr<ConnectionPool> pool;
};

std::unique_ptr<Forwarder> Forwarder::create(const ForwardConfig& config) {
    auto f = std::make_unique<Forwarder>();
    f->impl_ = std::make_unique<Impl>();
    f->impl_->config = config;
    f->impl_->pool = ConnectionPool::create(64);
    return f;
}

Forwarder::~Forwarder() = default;

static std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

static bool provider_disconnect_error(int error) {
    // Photon may report an incomplete chunked body with errno == 0. Treat it
    // the same as the usual socket reset errors for the public availability
    // contract; malformed bytes that were fully received remain protocol errors.
    return error == 0 || error == ECONNRESET || error == ECONNABORTED
        || error == EPIPE || error == ENOTCONN;
}

bool has_invalid_success_payload(int status_code, std::string_view content_type,
                                 std::string_view body) {
    if (status_code < 200 || status_code >= 300 || status_code == 204 || status_code == 205)
        return false;
    // A payload-bearing success with no body is never a valid provider
    // response.  Some servers lose the Content-Type header when the
    // connection is aborted after sending the status line.
    if (body.empty()) return true;

    auto media_type = lower(content_type);
    if (auto separator = media_type.find(';'); separator != std::string::npos)
        media_type.resize(separator);
    while (!media_type.empty() && std::isspace(static_cast<unsigned char>(media_type.back())))
        media_type.pop_back();
    const auto json_type = media_type == "application/json"
        || (media_type.size() > 5 && media_type.ends_with("+json"));
    if (!json_type) return false;

    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    return document.HasParseError();
}

bool is_event_stream_content_type(std::string_view content_type) {
    auto media_type = lower(content_type);
    if (auto separator = media_type.find(';'); separator != std::string::npos)
        media_type.resize(separator);
    while (!media_type.empty()
        && std::isspace(static_cast<unsigned char>(media_type.back()))) {
        media_type.pop_back();
    }
    size_t first = 0;
    while (first < media_type.size()
        && std::isspace(static_cast<unsigned char>(media_type[first]))) {
        ++first;
    }
    if (first > 0) media_type.erase(0, first);
    return media_type == "text/event-stream";
}

bool is_explicit_provider_rejection(const ForwardResult& result) {
    return result.provider_response_received && result.provider_status_code >= 400;
}

static bool hop_by_hop(std::string_view name) {
    const auto key = lower(name);
    return key == "connection" || key == "keep-alive" || key == "proxy-authenticate"
        || key == "proxy-authorization" || key == "te" || key == "trailer"
        || key == "transfer-encoding" || key == "upgrade";
}

static bool safe_request_header(std::string_view name) {
    if (hop_by_hop(name)) return false;
    const auto key = lower(name);
    return key == "accept" || key == "user-agent" || key == "x-request-id"
        || key == "idempotency-key" || key == "x-api-key";
}

static bool safe_response_header(std::string_view name,
                                 const dispatch::UpstreamTarget& target) {
    if (hop_by_hop(name)) return false;
    if (target.allowed_response_headers.empty()) {
        const auto key = lower(name);
        return key == "content-type" || key == "retry-after" || key == "x-request-id"
            || key == "openai-request-id" || key.starts_with("x-ratelimit-")
            || key.starts_with("ratelimit-");
    }
    const auto key = lower(name);
    for (const auto& allowed : target.allowed_response_headers) {
        if (key == lower(allowed)) return true;
    }
    return false;
}

static int as_int(const rapidjson::Value& object, const char* key) {
    if (!object.IsObject() || !object.HasMember(key)) return 0;
    const auto& value = object[key];
    if (value.IsInt()) return value.GetInt();
    if (value.IsInt64()) return static_cast<int>(value.GetInt64());
    if (value.IsUint()) return static_cast<int>(value.GetUint());
    return 0;
}

static bool valid_usage_count(const rapidjson::Value& value) {
    if (value.IsInt()) return value.GetInt() >= 0;
    if (value.IsInt64())
        return value.GetInt64() >= 0 && value.GetInt64() <= std::numeric_limits<int>::max();
    if (value.IsUint()) return value.GetUint() <= std::numeric_limits<int>::max();
    if (value.IsUint64()) return value.GetUint64() <= std::numeric_limits<int>::max();
    return false;
}

static bool usage_has_invalid_counts(const rapidjson::Value& usage) {
    constexpr std::string_view count_fields[] = {
        "input_tokens", "prompt_tokens", "output_tokens", "completion_tokens",
        "cache_creation_input_tokens", "cache_read_input_tokens",
        "promptTokenCount", "candidatesTokenCount", "cachedContentTokenCount",
        "reasoning_tokens", "thoughtsTokenCount"};
    for (const auto field : count_fields) {
        if (usage.HasMember(field.data()) && !valid_usage_count(usage[field.data()]))
            return true;
    }
    for (const auto details_name : {"prompt_tokens_details", "input_tokens_details",
                                    "output_tokens_details"}) {
        if (!usage.HasMember(details_name)) continue;
        const auto& details = usage[details_name];
        if (!details.IsObject()) return true;
        for (const auto field : {"cached_tokens", "reasoning_tokens"}) {
            if (details.HasMember(field) && !valid_usage_count(details[field]))
                return true;
        }
    }
    return false;
}

static void parse_usage(std::string_view body, ForwardResult& result) {
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject()) return;
    const rapidjson::Value* usage = nullptr;
    if (document.HasMember("usage") && document["usage"].IsObject()) usage = &document["usage"];
    if (!usage && document.HasMember("usageMetadata") && document["usageMetadata"].IsObject())
        usage = &document["usageMetadata"];
    if (!usage && document.HasMember("response") && document["response"].IsObject()
        && document["response"].HasMember("usage") && document["response"]["usage"].IsObject())
        usage = &document["response"]["usage"];
    if (!usage) return;
    result.malformed_usage = usage_has_invalid_counts(*usage);

    result.input_tokens = as_int(*usage, "input_tokens");
    if (result.input_tokens == 0) result.input_tokens = as_int(*usage, "prompt_tokens");
    if (result.input_tokens == 0) result.input_tokens = as_int(*usage, "promptTokenCount");
    result.output_tokens = as_int(*usage, "output_tokens");
    if (result.output_tokens == 0) result.output_tokens = as_int(*usage, "completion_tokens");
    if (result.output_tokens == 0) result.output_tokens = as_int(*usage, "candidatesTokenCount");
    result.cache_create_tokens = as_int(*usage, "cache_creation_input_tokens");
    result.cache_read_tokens = as_int(*usage, "cache_read_input_tokens");
    if (result.cache_read_tokens == 0)
        result.cache_read_tokens = as_int(*usage, "cachedContentTokenCount");
    result.reasoning_tokens = as_int(*usage, "reasoning_tokens");
    if (result.reasoning_tokens == 0) result.reasoning_tokens = as_int(*usage, "thoughtsTokenCount");
    if (usage->HasMember("prompt_tokens_details") && (*usage)["prompt_tokens_details"].IsObject())
        result.cache_read_tokens = std::max(result.cache_read_tokens,
            as_int((*usage)["prompt_tokens_details"], "cached_tokens"));
    if (usage->HasMember("input_tokens_details") && (*usage)["input_tokens_details"].IsObject())
        result.cache_read_tokens = std::max(result.cache_read_tokens,
            as_int((*usage)["input_tokens_details"], "cached_tokens"));
    if (usage->HasMember("output_tokens_details") && (*usage)["output_tokens_details"].IsObject())
        result.reasoning_tokens = std::max(result.reasoning_tokens,
            as_int((*usage)["output_tokens_details"], "reasoning_tokens"));
    if (document.HasMember("service_tier") && document["service_tier"].IsString())
        result.service_tier = document["service_tier"].GetString();
    // Preserve the provider payload for diagnostics and billing reconciliation;
    // the structured counters above are the authoritative numeric fields.
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    usage->Accept(writer);
    result.provider_usage_json.assign(buffer.GetString(),
        std::min<size_t>(buffer.GetSize(), 1024 * 1024));
}

ForwardResult Forwarder::forward(const dispatch::UpstreamTarget& target,
                                  const ForwardRequest& request,
                                  ProtocolMode protocol_mode) {
    ForwardResult result;

    std::string url = target.base_url;
    if (!target.upstream_path.empty()) {
        if (!url.empty() && url.back() != '/' && target.upstream_path.front() != '/')
            url += '/';
        url += target.upstream_path;
    }

    if (url.empty()) {
        result.status_code = 502;
        result.error = "empty upstream URL";
        return result;
    }

    auto* http_client = impl_->pool->get_client(target.base_url);
    if (!http_client) {
        result.status_code = 502;
        result.error = "HTTP client not available";
        return result;
    }

    auto verb = photon::net::http::string_to_verb(
        target.http_method.empty() ? request.method : target.http_method);
    if (verb == photon::net::http::Verb::UNKNOWN) verb = photon::net::http::Verb::POST;
    auto* op = http_client->new_operation(verb, url);

    op->timeout = {(request.stream ? impl_->config.first_token_timeout_ms
                                   : impl_->config.request_timeout_ms) * 1000ULL};

    for (auto& [key, value] : target.auth_headers) {
        op->req.headers.insert(key, value);
    }
    for (auto& [key, value] : target.request_headers) {
        if (!hop_by_hop(key)) op->req.headers.insert(key, value);
    }
    for (auto& [key, value] : request.headers) {
        if (safe_request_header(key)) op->req.headers.insert(key, value);
    }
    if (!request.accept.empty()) op->req.headers.insert("Accept", request.accept);
    if (!request.user_agent.empty()) op->req.headers.insert("User-Agent", request.user_agent);
    if (!request.request_id.empty()) op->req.headers.insert("X-Request-ID", request.request_id);
    if (!request.idempotency_key.empty()) op->req.headers.insert("Idempotency-Key", request.idempotency_key);

    if (!request.content_type.empty()) op->req.headers.insert("Content-Type", request.content_type);
    else if (!request.body.empty()) op->req.headers.insert("Content-Type", "application/json");
    if (!request.body.empty()) op->set_body(request.body);

    if (!target.proxy_url.empty()) {
        op->set_proxy(target.proxy_url);
    }

    auto start = std::chrono::steady_clock::now();
    int ret = op->call();

    if (ret < 0) {
        const auto call_errno = errno;
        const bool timed_out = call_errno == ETIMEDOUT;
        // A connection reset before Provider headers is an availability
        // failure. Keep the protocol-error contract reserved for a bounded
        // header timeout or malformed/incomplete Provider payload.
        result.status_code = timed_out ? 502 : 503;
        result.error = timed_out ? "provider response header timed out"
                                 : "upstream connection unavailable";
        result.body = timed_out
            ? R"({"error":{"type":"provider_protocol_error","message":"Provider did not return response headers before the first-token deadline"}})"
            : R"({"error":{"type":"provider_unavailable","message":"Provider connection was unavailable before a response was received"}})";
        if (request.stream && request.response_start)
            request.response_start(result.status_code, "application/json", {});
        LOG_ERROR("Forward to {} failed: {}", url, strerror(call_errno));
        http_client->destroy_operation(op);
        return result;
    }

    result.status_code = op->resp.status_code();
    result.provider_response_received = true;
    result.provider_status_code = result.status_code;
    result.content_type = std::string(op->resp.headers["Content-Type"]);
    for (auto header : op->resp.headers) {
        if (safe_response_header(header.first, target))
            result.response_headers.emplace_back(header.first, header.second);
        if (lower(header.first) == "retry-after") {
            char* end = nullptr;
            auto value = std::strtol(std::string(header.second).c_str(), &end, 10);
            if (end && *end == '\0' && value >= 0) result.retry_after_ms = static_cast<int>(value * 1000);
        }
    }
    const bool invalid_stream_content_type = request.stream
        && result.status_code < 400
        && !is_event_stream_content_type(result.content_type);
    if (invalid_stream_content_type) {
        result.status_code = 502;
        result.content_type = "application/json";
        result.error = "streaming upstream response did not use text/event-stream";
    }
    if (request.response_start)
        request.response_start(result.status_code, result.content_type, result.response_headers);

    // A stream request may still receive a normal JSON error response.  Do
    // not emit that body as SSE: no output has reached the client yet, so the
    // caller may safely abort and fail over the lease.
    if (!request.stream || result.status_code >= 400) {
        std::string resp_body;
        char buf[64 * 1024];
        while (true) {
            ssize_t n = op->resp.read(buf, sizeof(buf));
            if (n < 0) {
                const auto body_errno = errno;
                const auto unavailable = provider_disconnect_error(body_errno);
                result.status_code = unavailable ? 503 : 502;
                result.content_type = "application/json";
                result.error = unavailable
                    ? "provider response body connection unavailable"
                    : "provider response body read failed";
                resp_body = unavailable
                    ? R"({"error":{"type":"provider_unavailable","message":"Provider connection was unavailable before the response body completed"}})"
                    : R"({"error":{"type":"provider_protocol_error","message":"Provider response ended before the body was complete"}})";
                break;
            }
            if (n == 0) break;
            if (resp_body.size() + static_cast<size_t>(n) > impl_->config.max_response_body_size) {
                result.status_code = 502;
                result.error = "upstream response exceeded configured body limit";
                http_client->destroy_operation(op);
                return result;
            }
            resp_body.append(buf, n);
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        result.duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (has_invalid_success_payload(result.status_code, result.content_type, resp_body)) {
            result.status_code = 502;
            result.content_type = "application/json";
            result.error = "provider returned incomplete or invalid JSON";
            resp_body = R"({"error":{"type":"provider_protocol_error","message":"Provider returned incomplete or invalid JSON"}})";
        } else {
            parse_usage(resp_body, result);
        }

        result.stream = false;
        result.body = invalid_stream_content_type
            ? R"({"error":{"type":"provider_protocol_error","message":"Streaming provider response must use text/event-stream"}})"
            : std::move(resp_body);
    } else {
        StreamPipeConfig pipe_cfg{
            .read_buf_size = impl_->config.read_buf_size,
            .write_buf_size = impl_->config.write_buf_size,
            .first_token_timeout_ms = impl_->config.first_token_timeout_ms,
            .inter_chunk_timeout_ms = impl_->config.inter_chunk_timeout_ms,
            .total_timeout_ms = impl_->config.total_stream_timeout_ms,
            .keepalive_interval_ms = impl_->config.keepalive_interval_ms,
            .inject_keepalive = true,
        };

        StreamPipe pipe(pipe_cfg, protocol_mode,
                        request.stream_source, request.stream_target);

        auto upstream_read = [&](char* buf, size_t len) -> ssize_t {
            return op->resp.read(buf, len);
        };

        std::string accumulated;
        auto client_write = [&](const char* data, size_t len) -> ssize_t {
            ssize_t written;
            if (request.stream_write) {
                written = request.stream_write(data, len);
            } else {
                accumulated.append(data, len);
                written = static_cast<ssize_t>(len);
            }
            if (written > 0 && !result.output_started) {
                result.output_started = true;
                if (request.output_started) request.output_started();
            }
            return written;
        };

        auto stream_result = pipe.run(upstream_read, client_write);

        result.stream = true;
        result.input_tokens = stream_result.input_tokens;
        result.output_tokens = stream_result.output_tokens;
        result.cache_create_tokens = stream_result.cache_create_tokens;
        result.cache_read_tokens = stream_result.cache_read_tokens;
        result.reasoning_tokens = stream_result.reasoning_tokens;
        result.malformed_usage = stream_result.malformed_usage;
        result.provider_usage_json = std::move(stream_result.provider_usage_json);
        result.first_token_ms = stream_result.first_token_ms;
        result.duration_ms = stream_result.total_duration_ms;
        result.client_disconnect = stream_result.client_disconnect;
        result.stream_incomplete = stream_result.incomplete;
        result.stream_timeout = stream_result.timed_out;
        if (result.client_disconnect) {
            result.disconnect_reason = "client_disconnect";
            result.cancellation_reason = result.output_started
                ? "client_disconnect_after_output" : "client_disconnect_before_output";
            if (!result.output_started) {
                result.status_code = 499;
                result.error = "client disconnected before response output";
            } else if (result.stream_incomplete) {
                result.status_code = 502;
                result.error = "client disconnected before provider stream completion";
            }
        } else if (result.stream_incomplete) {
            const auto provider_unavailable = !result.stream_timeout;
            result.status_code = provider_unavailable ? 503 : 502;
            result.content_type = "application/json";
            result.disconnect_reason = result.stream_timeout
                ? "provider_timeout" : "provider_disconnect";
            result.cancellation_reason = "provider_stream_incomplete";
            result.error = result.stream_timeout
                ? "provider stream timed out before a terminal event"
                : "provider stream ended before a terminal event";
            if (!result.output_started) {
                result.body = provider_unavailable
                    ? R"({"error":{"type":"provider_unavailable","message":"Provider connection was unavailable before the stream completed"}})"
                    : R"({"error":{"type":"provider_protocol_error","message":"Provider stream ended before completion"}})";
            }
        }
    }

    http_client->destroy_operation(op);
    return result;
}

}  // namespace gateway::forwarder
