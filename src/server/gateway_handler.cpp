#include "server/gateway_handler.h"
#include "cache/garnet_keyspace.h"
#include "platform/logging.h"
#include "platform/metrics.h"
#include "platform/fault_injection.h"
#include "forwarder/retry_policy.h"
#include "forwarder/forwarder.h"

#include <photon/thread/thread.h>
#include <xxhash.h>

#include <chrono>
#include <format>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <photon/net/http/websocket.h>
#include <photon/net/http/client.h>
#include <photon/thread/thread11.h>

namespace gateway::server {

namespace {
int64_t parse_auth_version(std::string_view json) {
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("version")) return 0;
    const auto& value = doc["version"];
    if (value.IsInt64()) return value.GetInt64();
    if (value.IsUint64()) return static_cast<int64_t>(value.GetUint64());
    return 0;
}

std::string extract_model(std::string_view body) {
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("model")
        || !doc["model"].IsString()) return {};
    return doc["model"].GetString();
}

bool is_json_request(const HttpRequest& req) {
    return req.content_type.empty() || req.content_type.starts_with("application/json")
        || req.content_type.starts_with("text/json");
}

bool valid_json_object(std::string_view body) {
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());
    return !doc.HasParseError() && doc.IsObject();
}

std::string error_json(std::string_view type, std::string_view message) {
    rapidjson::Document document;
    document.SetObject();
    auto& alloc = document.GetAllocator();
    rapidjson::Value error(rapidjson::kObjectType);
    error.AddMember("type", rapidjson::Value(type.data(), type.size(), alloc), alloc);
    error.AddMember("message", rapidjson::Value(message.data(), message.size(), alloc), alloc);
    document.AddMember("error", error, alloc);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

std::string photon_websocket_url(std::string url) {
    // Photon performs the WebSocket upgrade through its HTTP client and expects
    // an HTTP(S) URL even though the Platform contract names it ws/wss.
    if (url.starts_with("wss://")) url.replace(0, 6, "https://");
    else if (url.starts_with("ws://")) url.replace(0, 5, "http://");
    return url;
}

protocol::Format format_from_name(std::string_view name, protocol::Format fallback) {
    if (name == "anthropic" || name == "messages") return protocol::Format::Anthropic;
    if (name == "openai_chat" || name == "chat_completions") return protocol::Format::OpenAIChatCompletions;
    if (name == "openai_responses" || name == "responses") return protocol::Format::OpenAIResponses;
    if (name == "gemini") return protocol::Format::Gemini;
    if (name == "grok" || name == "xai") return protocol::Format::OpenAIChatCompletions;
    return fallback;
}

bool chat_capability(Capability capability) {
    return capability == Capability::Messages || capability == Capability::ChatCompletions
        || capability == Capability::Responses || capability == Capability::ResponsesSubpath
        || capability == Capability::GeminiGenerate;
}

std::string media_action(std::string_view operation) {
    if (operation.ends_with("list")) return "list";
    if (operation.ends_with("cancel")) return "cancel";
    if (operation.ends_with("content") || operation.ends_with("download")) return "content";
    if (operation.ends_with("delete_outputs")) return "delete_outputs";
    if (operation.ends_with("delete")) return "delete";
    if (operation.ends_with("items")) return "items";
    return "get";
}

bool media_control_operation(std::string_view operation) {
    return operation == "images_task_get" || operation == "images_batch_get"
        || operation == "images_batch_list"
        || operation == "images_batch_items" || operation == "images_batch_download"
        || operation == "images_batch_cancel" || operation == "images_batch_delete"
        || operation == "images_batch_delete_outputs" || operation == "images_batch_item_content"
        || operation == "videos_get" || operation == "videos_content"
        || operation == "videos_cancel" || operation == "videos_delete"
        || operation == "videos_delete_outputs";
}

std::string media_operation_id(std::string_view path) {
    constexpr std::string_view markers[] = {
        "/images/tasks/", "/images/batches/", "/videos/"
    };
    for (auto marker : markers) {
        auto position = path.find(marker);
        if (position == std::string_view::npos) continue;
        auto id = path.substr(position + marker.size());
        auto slash = id.find('/');
        if (slash != std::string_view::npos) id = id.substr(0, slash);
        return std::string(id);
    }
    return {};
}

std::string media_view_json(const dispatch::MediaOperationResult& result) {
    rapidjson::Document output;
    output.SetObject();
    auto& alloc = output.GetAllocator();
    output.AddMember("id", rapidjson::Value(result.operation_id.c_str(), alloc), alloc);
    output.AddMember("task_id", rapidjson::Value(result.operation_id.c_str(), alloc), alloc);
    output.AddMember("object", rapidjson::Value("media.operation", alloc), alloc);
    output.AddMember("type", rapidjson::Value(result.operation_type.c_str(), alloc), alloc);
    output.AddMember("status", rapidjson::Value(result.status.c_str(), alloc), alloc);
    output.AddMember("progress", result.progress, alloc);
    if (!result.upstream_task_id.empty())
        output.AddMember("upstream_task_id",
            rapidjson::Value(result.upstream_task_id.c_str(), alloc), alloc);
    if (!result.output_url.empty())
        output.AddMember("url", rapidjson::Value(result.output_url.c_str(), alloc), alloc);
    if (!result.content_type.empty())
        output.AddMember("content_type", rapidjson::Value(result.content_type.c_str(), alloc), alloc);
    if (!result.output_metadata.empty()) {
        rapidjson::Document metadata;
        metadata.Parse(result.output_metadata.c_str(), result.output_metadata.size());
        if (!metadata.HasParseError()) {
            rapidjson::Value value(metadata, alloc);
            output.AddMember("output", value, alloc);
        }
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    output.Accept(writer);
    return buffer.GetString();
}

std::string media_items_json(const dispatch::MediaOperationResult& result) {
    rapidjson::Document metadata;
    metadata.Parse(result.output_metadata.c_str(), result.output_metadata.size());
    if (metadata.HasParseError() || !metadata.IsObject()
        || !metadata.HasMember("data") || !metadata["data"].IsArray())
        return "[]";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    metadata["data"].Accept(writer);
    return buffer.GetString();
}

std::string provider_task_id(std::string_view body) {
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject()) return {};
    constexpr const char* keys[] = {"task_id", "request_id", "id"};
    for (const auto* key : keys) {
        if (document.HasMember(key) && document[key].IsString())
            return document[key].GetString();
    }
    return {};
}

std::string provider_media_status(std::string_view body, std::string_view task_id) {
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (!document.HasParseError() && document.IsObject()) {
        if (document.HasMember("status") && document["status"].IsString()) {
            std::string status = document["status"].GetString();
            std::transform(status.begin(), status.end(), status.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (status == "succeeded" || status == "completed") return "succeeded";
            if (status == "failed" || status == "error") return "failed";
            if (status == "canceled" || status == "cancelled") return "canceled";
        }
        if (task_id.empty() && (document.HasMember("data") || document.HasMember("url")))
            return "succeeded";
    }
    return "running";
}

std::string provider_output_url(std::string_view body) {
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject()) return {};
    for (const char* key : {"output_url", "url"}) {
        if (document.HasMember(key) && document[key].IsString()) return document[key].GetString();
    }
    if (document.HasMember("data") && document["data"].IsArray()) {
        for (const auto& value : document["data"].GetArray()) {
            if (value.IsObject() && value.HasMember("url") && value["url"].IsString())
                return value["url"].GetString();
        }
    }
    return {};
}

void accumulate_realtime_usage(std::string_view frame,
                               std::atomic<int>& input_tokens,
                               std::atomic<int>& output_tokens,
                               std::atomic<int>& cache_tokens,
                               std::atomic<int>& reasoning_tokens) {
    rapidjson::Document document;
    document.Parse(frame.data(), frame.size());
    if (document.HasParseError() || !document.IsObject()) return;
    const rapidjson::Value* usage = nullptr;
    if (document.HasMember("usage") && document["usage"].IsObject()) usage = &document["usage"];
    if (!usage && document.HasMember("response") && document["response"].IsObject()
        && document["response"].HasMember("usage") && document["response"]["usage"].IsObject())
        usage = &document["response"]["usage"];
    if (!usage) return;
    auto read = [&](std::initializer_list<const char*> keys) {
        for (const auto* key : keys) {
            if (!usage->HasMember(key)) continue;
            const auto& value = (*usage)[key];
            if (value.IsInt()) return std::max(0, value.GetInt());
            if (value.IsUint()) return static_cast<int>(value.GetUint());
        }
        return 0;
    };
    input_tokens.store(std::max(input_tokens.load(),
        read({"input_tokens", "prompt_tokens"})), std::memory_order_relaxed);
    output_tokens.store(std::max(output_tokens.load(),
        read({"output_tokens", "completion_tokens"})), std::memory_order_relaxed);
    cache_tokens.store(std::max(cache_tokens.load(),
        read({"cache_read_input_tokens", "cached_tokens"})), std::memory_order_relaxed);
    reasoning_tokens.store(std::max(reasoning_tokens.load(),
        read({"reasoning_tokens"})), std::memory_order_relaxed);
}
}

GatewayHandler::GatewayHandler(cache::GarnetClient& garnet,
                               dispatch::CapnpDispatchClient& dispatch,
                               usage::UsageCollector& collector,
                               auth::SpeculativeCache& auth_cache)
    : garnet_(garnet),
      dispatch_(dispatch),
      collector_(collector),
      auth_cache_(auth_cache),
      api_key_auth_(auth_cache),
      forwarder_(forwarder::Forwarder::create({})) {}

int GatewayHandler::bridge_realtime(const HttpRequest& req,
                                    photon::net::http::IWebSocketStream& client) {
    using photon::net::http::WebSocketOpcode;
    constexpr size_t kMaxFrame = 1 * 1024 * 1024;
    constexpr uint64_t kFirstFrameTimeoutUs = 5'000'000;
    std::string first(kMaxFrame, '\0');
    WebSocketOpcode first_opcode = WebSocketOpcode::Text;
    auto first_size = client.recv_frame(first.data(), first.size(), &first_opcode,
                                        kFirstFrameTimeoutUs);
    if (first_size <= 0 || (first_opcode != WebSocketOpcode::Text
                            && first_opcode != WebSocketOpcode::Binary)) {
        client.close(photon::net::http::WebSocketCloseCode::ProtocolError,
                     "first realtime event must be JSON");
        return -1;
    }
    first.resize(static_cast<size_t>(first_size));
    auto parsed = protocol::Converter::parse(first, protocol::Format::OpenAIResponses);
    if (parsed.model.empty()) parsed.model = protocol::Converter::parse_realtime_model(first);
    if (parsed.model.empty()) {
        client.send_text(R"({"type":"error","error":{"code":"invalid_request","message":"model is required"}})");
        client.close(photon::net::http::WebSocketCloseCode::InvalidFramePayloadData,
                     "model is required");
        return -1;
    }

    auto raw_key = extract_api_key(req);
    if (raw_key.empty()) {
        client.close(photon::net::http::WebSocketCloseCode::PolicyViolation,
                     "missing API key");
        return -1;
    }
    auto key_hash = auth::ApiKeyAuth::hash_key(raw_key);
    int64_t cached_version = 0;
    if (auto hit = auth_cache_.lookup(key_hash)) cached_version = hit->version;
    else {
        auto cached = garnet_.get(cache::keyspace::auth(key_hash));
        if (cached.found) cached_version = parse_auth_version(cached.value);
    }
    auto now = std::chrono::steady_clock::now();
    auto request_id = req.request_id.empty()
        ? std::format("{:016x}", XXH64(&now, sizeof(now), 0)) : std::string(req.request_id);
    auto session_hash = compute_session_hash(key_hash, parsed.metadata_user_id, first, parsed.model);
    dispatch::DispatchRequest dispatch_req{
        .api_key_hash = key_hash, .requested_model = parsed.model,
        .session_hash = session_hash, .client_ip = std::string(req.client_ip),
        .request_id = request_id, .cached_auth_version = cached_version,
        .endpoint = static_cast<int>(dispatch::DispatchRequest::EndpointKind::Realtime),
        .metadata_user_id = parsed.metadata_user_id, .stream = true,
        .operation = "realtime_session", .inbound_format = "openai_responses",
        .http_method = "GET", .request_path = std::string(req.path),
        .content_type = "application/json", .capability = "realtime",
        .idempotency_key = std::string(req.idempotency_key), .realtime_session = true,
        .request_query = std::string(req.query),
    };
    auto dispatch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    int dispatch_transport_retries = 0;
    dispatch::DispatchResult dispatched;
    while (true) {
        dispatched = dispatch_.dispatch(dispatch_req);
        if (dispatched.outcome == dispatch::DispatchResult::Outcome::Ok
            || dispatched.outcome == dispatch::DispatchResult::Outcome::Reauth) {
            break;
        }
        if (!dispatch::is_retryable_platform_dispatch(dispatched)
            || ++dispatch_transport_retries > 8
            || std::chrono::steady_clock::now() >= dispatch_deadline) {
            break;
        }
        photon::thread_usleep(static_cast<uint64_t>(
            dispatch::platform_dispatch_retry_delay_ms(dispatch_transport_retries)) * 1000);
    }
    if (dispatched.outcome != dispatch::DispatchResult::Outcome::Ok
        && dispatched.outcome != dispatch::DispatchResult::Outcome::Reauth) {
        client.send_text(R"({"type":"error","error":{"code":"provider_unavailable","message":"No realtime provider is available"}})");
        client.close(photon::net::http::WebSocketCloseCode::PolicyViolation,
                     "provider unavailable");
        return -1;
    }
    auto& target = dispatched.upstream;

    if (!forwarder::validate_target_auth_headers(target.auth_headers)) {
        dispatch_.abort(dispatched.lease_token, "invalid_upstream_auth_headers");
        client.send_text(R"({"type":"error","error":{"code":"provider_protocol_error","message":"Provider authentication contract is invalid"}})");
        client.close(photon::net::http::WebSocketCloseCode::InternalServerError,
                     "invalid upstream auth headers");
        return -1;
    }

    auto upstream_url = photon_websocket_url(target.websocket_url);
    if (upstream_url.empty()) {
        upstream_url = target.base_url;
        upstream_url += target.upstream_path.empty() ? "/v1/responses" : target.upstream_path;
        upstream_url = photon_websocket_url(std::move(upstream_url));
    }
    auto* http_client = photon::net::http::new_http_client();
    if (!http_client) {
        dispatch_.abort(dispatched.lease_token, "realtime_connect_failed");
        client.close(photon::net::http::WebSocketCloseCode::InternalServerError,
                     "upstream client unavailable");
        return -1;
    }
    for (const auto& [key, value] : target.auth_headers)
        http_client->common_headers()->insert(key, value);
    for (const auto& [key, value] : target.request_headers)
        http_client->common_headers()->insert(key, value);
    if (!target.websocket_protocol.empty())
        http_client->common_headers()->insert("Sec-WebSocket-Protocol", target.websocket_protocol);
    if (!req.user_agent.empty()) http_client->set_user_agent(req.user_agent);
    if (!target.proxy_url.empty()) {
        if (!target.proxy_username.empty()) {
            // Build authenticated proxy URL: scheme://user:pass@host:port
            auto scheme_end = target.proxy_url.find("://");
            if (scheme_end != std::string::npos) {
                auto host_start = scheme_end + 3;
                std::string auth_url;
                auth_url.reserve(target.proxy_url.size()
                    + target.proxy_username.size() + target.proxy_password.size() + 4);
                auth_url.append(target.proxy_url, 0, host_start);
                auth_url.append(target.proxy_username);
                auth_url += ':';
                auth_url.append(target.proxy_password);
                auth_url += '@';
                auth_url.append(target.proxy_url, host_start, std::string::npos);
                http_client->set_proxy(auth_url);
            } else {
                http_client->set_proxy(target.proxy_url);
            }
        } else {
            http_client->set_proxy(target.proxy_url);
        }
    }
    collector_.record_evidence(
        dispatched.lease_token, "forwarded",
        "gateway", "realtime Provider handshake authorized");
    const auto forwarded_ack = dispatch_.record_lease_evidence(
        dispatched.lease_token, dispatch::LeaseEvidenceStage::Forwarded,
        "realtime Provider handshake authorized");
    if (forwarded_ack.acknowledged()) {
        collector_.acknowledge_evidence(
            dispatched.lease_token, "forwarded");
    }
    if (!forwarded_ack.acknowledged()) {
        delete http_client;
        dispatch_.abort(dispatched.lease_token, "forward_evidence_unavailable");
        client.close(photon::net::http::WebSocketCloseCode::InternalServerError,
                     "dispatch evidence unavailable");
        return -1;
    }
    auto* upstream = photon::net::http::websocket_connect(http_client, upstream_url, 30'000'000);
    if (!upstream) {
        delete http_client;
        dispatch_.abort(dispatched.lease_token, "realtime_connect_failed",
            dispatch::LeaseAbortDisposition::Unknown);
        client.send_text(R"({"type":"error","error":{"code":"provider_unavailable","message":"Realtime provider handshake failed"}})");
        client.close(photon::net::http::WebSocketCloseCode::InternalServerError,
                     "upstream handshake failed");
        return -1;
    }
    const auto initial_send = first_opcode == WebSocketOpcode::Binary
        ? upstream->send_binary(first.data(), first.size()) : upstream->send_text(first);
    if (initial_send <= 0) {
        dispatch_.abort(dispatched.lease_token, "realtime_initial_send_failed",
            dispatch::LeaseAbortDisposition::Unknown);
        upstream->close(photon::net::http::WebSocketCloseCode::InternalServerError);
        delete upstream;
        delete http_client;
        client.close(photon::net::http::WebSocketCloseCode::InternalServerError,
                     "upstream send failed");
        return -1;
    }

    std::atomic<bool> done{false};
    std::atomic<bool> client_disconnected{false};
    std::atomic<int> frames{1};
    std::atomic<int> input_tokens{0};
    std::atomic<int> output_tokens{0};
    std::atomic<int> cache_tokens{0};
    std::atomic<int> reasoning_tokens{0};
    std::atomic<bool> output_evidence_recorded{false};
    auto upstream_thread = photon::thread_enable_join(photon::thread_create11([&] {
        std::string buffer(kMaxFrame, '\0');
        while (!done.load(std::memory_order_relaxed)) {
            WebSocketOpcode opcode = WebSocketOpcode::Text;
            auto n = upstream->recv_frame(buffer.data(), buffer.size(), &opcode, 30'000'000);
            if (n <= 0 || opcode == WebSocketOpcode::Close) break;
            ++frames;
            ssize_t sent = -1;
            if (opcode == WebSocketOpcode::Binary)
                sent = client.send_binary(buffer.data(), static_cast<size_t>(n));
            else if (opcode == WebSocketOpcode::Text) {
                auto frame = std::string_view(buffer.data(), static_cast<size_t>(n));
                accumulate_realtime_usage(frame, input_tokens, output_tokens,
                    cache_tokens, reasoning_tokens);
                sent = client.send_text(frame);
            }
            if (sent > 0 && !output_evidence_recorded.exchange(true)) {
                collector_.record_evidence(
                    dispatched.lease_token, "output_started",
                    "gateway", "first realtime frame written to client");
                const auto ack = dispatch_.record_lease_evidence(
                    dispatched.lease_token, dispatch::LeaseEvidenceStage::OutputStarted,
                    "first realtime frame written to client");
                if (ack.acknowledged()) {
                    collector_.acknowledge_evidence(
                        dispatched.lease_token, "output_started");
                } else {
                    LOG_WARN("Realtime output evidence RPC failed, retained for retry: request={} lease={} error={}",
                             request_id, dispatched.lease_token, ack.error_code);
                }
            }
        }
        done.store(true, std::memory_order_relaxed);
        client.close(photon::net::http::WebSocketCloseCode::NormalClosure);
    }));
    std::string buffer(kMaxFrame, '\0');
    while (!done.load(std::memory_order_relaxed)) {
        WebSocketOpcode opcode = WebSocketOpcode::Text;
        auto n = client.recv_frame(buffer.data(), buffer.size(), &opcode, 30'000'000);
        if (n < 0 || opcode == WebSocketOpcode::Close) {
            client_disconnected.store(true, std::memory_order_relaxed);
            break;
        }
        if (n == 0) break;
        ++frames;
        if (opcode == WebSocketOpcode::Binary) upstream->send_binary(buffer.data(), static_cast<size_t>(n));
        else if (opcode == WebSocketOpcode::Text) upstream->send_text(std::string_view(buffer.data(), static_cast<size_t>(n)));
    }
    done.store(true, std::memory_order_relaxed);
    upstream->close(photon::net::http::WebSocketCloseCode::NormalClosure);
    photon::thread_join(upstream_thread);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - now).count();
    const bool disconnected = client_disconnected.load(std::memory_order_relaxed);
    collector_.record(usage::UsageEvent{
        .lease_token = dispatched.lease_token, .request_id = request_id,
        .api_key_id = dispatched.api_key_id, .user_id = target.user_id,
        .account_id = target.account_id, .group_id = target.group_id,
        .model = parsed.model, .upstream_model = target.mapped_model,
        .input_tokens = input_tokens.load(), .output_tokens = output_tokens.load(),
        .cache_read_tokens = cache_tokens.load(),
        .duration_ms = static_cast<int>(duration), .stream = true,
        .client_disconnect = disconnected, .status_code = 101,
        .realtime_duration_ms = static_cast<int>(duration), .realtime_frames = frames.load(),
        .disconnect_reason = disconnected ? "client_disconnect" : "normal",
        .reasoning_tokens = reasoning_tokens.load(),
        .upstream_endpoint = target.upstream_path,
        .pricing_version = "v1",
    });
    delete upstream;
    delete http_client;
    return 0;
}

int GatewayHandler::handle(const HttpRequest& req, HttpResponse& resp,
                           const MatchedCapability& matched) {
    const auto& spec = *matched.spec;
    const auto endpoint = spec.endpoint;
    auto& metrics = platform::global_metrics();
    metrics.requests_total.fetch_add(1, std::memory_order_relaxed);
    metrics.active_connections.fetch_add(1, std::memory_order_relaxed);

    auto start = std::chrono::steady_clock::now();
    std::string request_id = req.request_id.empty()
        ? std::format("{:016x}", XXH64(&start, sizeof(start), 0))
        : std::string(req.request_id);
    resp.headers.emplace_back("X-Request-ID", request_id);

    if (!is_safe_query_string(req.query)) {
        resp.status_code = 400;
        resp.body = R"({"error":{"type":"invalid_request_error","message":"Malformed query string"}})";
        metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    // --- Step 1: Extract API key and authenticate ---
    auto raw_key = extract_api_key(req);
    if (raw_key.empty()) {
        resp.status_code = 401;
        resp.body = R"({"error":{"type":"authentication_error","message":"Missing API key"}})";
        metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    auto key_hash = auth::ApiKeyAuth::hash_key(raw_key);
    auto fingerprint_seed = XXH64(req.method.data(), req.method.size(), 0);
    fingerprint_seed = XXH64(req.path.data(), req.path.size(), fingerprint_seed);
    fingerprint_seed = XXH64(req.query.data(), req.query.size(), fingerprint_seed);
    fingerprint_seed = XXH64(req.content_type.data(), req.content_type.size(), fingerprint_seed);
    const auto request_fingerprint = std::format("{:016x}",
        XXH64(req.body.data(), req.body.size(), fingerprint_seed));

    if (media_control_operation(matched.operation)) {
        auto action = media_action(matched.operation);
        auto result = dispatch_.media_operation(dispatch::MediaOperationRequest{
            .api_key_hash = key_hash,
            .operation_id = media_operation_id(req.path),
            .action = action,
            .request_id = request_id,
            .client_ip = std::string(req.client_ip),
            .idempotency_key = std::string(req.idempotency_key),
            .request_fingerprint = request_fingerprint,
        });
        resp.status_code = result.status_code;
        if (!result.accepted) {
            resp.body = error_json(result.error_code, result.error_message);
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        } else if (action == "list") {
            resp.status_code = 200;
            resp.body = result.output_metadata.empty()
                ? R"({"object":"list","data":[],"has_more":false})"
                : result.output_metadata;
            resp.headers.emplace_back("Content-Type", "application/json");
            resp.headers.emplace_back("Cache-Control", "no-store");
        } else if (action == "items") {
            resp.status_code = 200;
            resp.body = media_items_json(result);
            resp.headers.emplace_back("Content-Type", "application/json");
            resp.headers.emplace_back("Cache-Control", "no-store");
        } else if (action == "content") {
            if (result.output_url.empty()) {
                resp.status_code = 409;
                resp.body = error_json("output_not_ready", "Media output is not available");
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            } else {
                resp.status_code = 302;
                resp.headers.emplace_back("Location", result.output_url);
                resp.headers.emplace_back("Cache-Control", "no-store");
            }
        } else if (result.status_code != 204) {
            resp.body = media_view_json(result);
            if (result.status == "pending" || result.status == "running")
                resp.headers.emplace_back("Retry-After", "3");
            resp.headers.emplace_back("Cache-Control", "no-store");
        }
        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    const bool persistent_create = matched.operation == "images_generations_async"
        || matched.operation == "images_edits_async" || matched.operation == "images_batch_create"
        || matched.operation == "videos_generations" || matched.operation == "videos_edits"
        || matched.operation == "videos_extensions";
    if (persistent_create && !req.idempotency_key.empty()) {
        auto existing = dispatch_.media_operation(dispatch::MediaOperationRequest{
            .api_key_hash = key_hash,
            .action = "lookup_idempotency",
            .request_id = request_id,
            .client_ip = std::string(req.client_ip),
            .idempotency_key = std::string(req.idempotency_key),
            .request_fingerprint = request_fingerprint,
        });
        if (existing.accepted) {
            resp.status_code = existing.status == "pending" || existing.status == "running" ? 202 : 200;
            resp.body = media_view_json(existing);
            resp.headers.emplace_back("Cache-Control", "no-store");
            if (resp.status_code == 202) resp.headers.emplace_back("Retry-After", "3");
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (existing.status_code == 409) {
            resp.status_code = 409;
            resp.body = R"({"error":{"type":"idempotency_conflict","message":"Idempotency key was already used for a different request"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (existing.status_code != 404) {
            resp.status_code = existing.status_code > 0 ? existing.status_code : 503;
            resp.body = R"({"error":{"type":"platform_unavailable","message":"Media idempotency lookup failed"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
    }

    // --- Step 2: Two-tier auth cache lookup ---
    int64_t cached_version = 0;

    auto cache_hit = auth_cache_.lookup(key_hash);
    if (cache_hit) {
        cached_version = cache_hit->version;
        metrics.garnet_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        std::string garnet_key = cache::keyspace::auth(key_hash);
        auto garnet_resp = garnet_.get(garnet_key);
        if (garnet_resp.found) {
            metrics.garnet_hits.fetch_add(1, std::memory_order_relaxed);
            cached_version = parse_auth_version(garnet_resp.value);
        } else {
            metrics.garnet_misses.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // --- Step 3: Parse request body according to the registry ---
    const auto inbound_format = spec.inbound_format;
    protocol::ParsedRequest parsed;
    parsed.format = inbound_format;
    if (chat_capability(spec.capability) && is_json_request(req)) {
        parsed = protocol::Converter::parse(req.body, inbound_format);
    } else {
        parsed.model = extract_model(req.body);
        if (parsed.model.empty() && req.content_type.starts_with("multipart/form-data"))
            parsed.model = protocol::Converter::extract_multipart_field(
                req.body, req.content_type, "model");
    }
    if ((spec.capability == Capability::GeminiGenerate || spec.capability == Capability::GeminiModels)
        && parsed.model.empty()) {
        auto marker = req.path.find("/models/");
        if (marker != std::string_view::npos) {
            auto start = marker + 8;
            auto end = req.path.find(':', start);
            if (end == std::string_view::npos) end = req.path.size();
            parsed.model = std::string(req.path.substr(start, end - start));
        }
    }
    const bool needs_model = (chat_capability(spec.capability)
        && spec.capability != Capability::ResponsesSubpath)
        || spec.capability == Capability::Embeddings
        || spec.capability == Capability::CountTokens
        || matched.operation == "images_generations" || matched.operation == "images_edits"
        || matched.operation == "images_generations_async" || matched.operation == "images_edits_async"
        || matched.operation == "images_batch_create" || matched.operation == "videos_generations"
        || matched.operation == "videos_edits" || matched.operation == "videos_extensions"
        || matched.operation == "audio_speech" || matched.operation == "audio_transcriptions";
    if (needs_model && ((is_json_request(req) && !valid_json_object(req.body))
        || parsed.model.empty())) {
        resp.status_code = 400;
        resp.body = R"({"error":{"type":"invalid_request_error","message":"A JSON object with a model is required"}})";
        metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }
    if (spec.capability == Capability::Embeddings && is_json_request(req)) {
        auto validation = protocol::Converter::validate_embeddings_request(req.body);
        if (!validation.valid) {
            resp.status_code = 400;
            resp.body = error_json("invalid_request_error", validation.message);
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
    }
    if (spec.capability == Capability::Search && is_json_request(req)) {
        rapidjson::Document search_doc;
        search_doc.Parse(req.body.data(), req.body.size());
        if (search_doc.HasParseError() || !search_doc.IsObject()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"A JSON object with a query is required"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (!search_doc.HasMember("query") || !search_doc["query"].IsString()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"query is required and must be a string"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        auto query_str = std::string_view(search_doc["query"].GetString());
        if (query_str.empty() || query_str.size() > 1000) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"query must be between 1 and 1000 characters"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (search_doc.HasMember("domain") && !search_doc["domain"].IsString()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"domain must be a string"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (search_doc.HasMember("recency") && !search_doc["recency"].IsString()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"recency must be a string"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
    }
    if (spec.capability == Capability::AudioTts && is_json_request(req)) {
        rapidjson::Document tts_doc;
        tts_doc.Parse(req.body.data(), req.body.size());
        if (tts_doc.HasParseError() || !tts_doc.IsObject()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"A JSON object with input and voice is required"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (!tts_doc.HasMember("input") || !tts_doc["input"].IsString()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"input is required and must be a string"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        auto input_str = std::string_view(tts_doc["input"].GetString());
        if (input_str.empty() || input_str.size() > 4096) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"input must be between 1 and 4096 characters"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (!tts_doc.HasMember("voice") || !tts_doc["voice"].IsString()) {
            resp.status_code = 400;
            resp.body = R"({"error":{"type":"invalid_request_error","message":"voice is required and must be a string"}})";
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }
        if (tts_doc.HasMember("response_format")) {
            if (!tts_doc["response_format"].IsString()) {
                resp.status_code = 400;
                resp.body = R"({"error":{"type":"invalid_request_error","message":"response_format must be a string"}})";
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                return 0;
            }
            auto fmt = std::string_view(tts_doc["response_format"].GetString());
            if (fmt != "mp3" && fmt != "opus" && fmt != "aac" && fmt != "flac"
                && fmt != "wav" && fmt != "pcm") {
                resp.status_code = 400;
                resp.body = R"({"error":{"type":"invalid_request_error","message":"response_format must be one of: mp3, opus, aac, flac, wav, pcm"}})";
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    if (spec.capability == Capability::AudioStt) {
        if (is_json_request(req)) {
            rapidjson::Document stt_doc;
            stt_doc.Parse(req.body.data(), req.body.size());
            if (stt_doc.HasParseError() || !stt_doc.IsObject()) {
                resp.status_code = 400;
                resp.body = R"({"error":{"type":"invalid_request_error","message":"A JSON object is required"}})";
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                return 0;
            }
            if (stt_doc.HasMember("language") && !stt_doc["language"].IsString()) {
                resp.status_code = 400;
                resp.body = R"({"error":{"type":"invalid_request_error","message":"language must be a string"}})";
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                return 0;
            }
            if (stt_doc.HasMember("language")) {
                auto lang = std::string_view(stt_doc["language"].GetString());
                if (lang.size() > 5) {
                    resp.status_code = 400;
                    resp.body = R"({"error":{"type":"invalid_request_error","message":"language must be a BCP-47 tag of at most 5 characters"}})";
                    metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                    metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                    return 0;
                }
            }
        }
    }
    bool is_stream = spec.can_stream && (parsed.stream
        || matched.operation == "streamGenerateContent");
    if (spec.realtime) is_stream = false;

    const auto media_request_usage = protocol::Converter::parse_media_request(
        req.body, req.content_type, matched.operation);

    if (is_stream) {
        metrics.requests_streaming.fetch_add(1, std::memory_order_relaxed);
        if (req.set_client_timeout_us) {
            req.set_client_timeout_us(static_cast<uint64_t>(req.stream_timeout_ms) * 1000);
        }
    }

    // --- Step 4: Compute session hash for sticky scheduling ---
    auto session_hash = compute_session_hash(
        key_hash, parsed.metadata_user_id, req.body, parsed.model);

    // --- Step 5+6+7: Dispatch, forward, and failover loop ---
    dispatch::DispatchRequest dispatch_req{
        .api_key_hash = key_hash,
        .requested_model = parsed.model,
        .session_hash = session_hash,
        .client_ip = std::string(req.client_ip),
        .request_id = request_id,
        .excluded_accounts = {},
        .cached_auth_version = cached_version,
        .endpoint = static_cast<int>(endpoint),
        .metadata_user_id = parsed.metadata_user_id,
        .stream = is_stream,
        .operation = std::string(matched.operation),
        .inbound_format = std::format("{}", static_cast<int>(inbound_format)),
        .http_method = std::string(req.method),
        .request_path = std::string(req.path),
        .content_type = std::string(req.content_type),
        .capability = std::string(spec.name),
        .idempotency_key = std::string(req.idempotency_key),
        .realtime_session = spec.realtime,
        .force_platform = std::string(matched.force_platform),
        .request_fingerprint = request_fingerprint,
        .request_query = std::string(req.query),
        .request_body = std::string(req.body),
    };

    forwarder::FailoverController failover;
    forwarder::RetryPolicy retry_policy;
    forwarder::ForwardResult forward_result;
    dispatch::DispatchResult dispatch_result;
    protocol::Format last_upstream_format = inbound_format;

    auto dispatch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    int dispatch_waits = 0;
    int dispatch_transport_retries = 0;
    int dispatch_retry_sequence = 0;
    bool terminal_abort = false;
    while (true) {
        // Dispatch
        while (true) {
            dispatch_result = dispatch_.dispatch(dispatch_req);
            metrics.dispatch_calls.fetch_add(1, std::memory_order_relaxed);

            if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Ok ||
                dispatch_result.outcome == dispatch::DispatchResult::Outcome::Reauth) {
                if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Reauth) {
                    auth_cache_.evict(key_hash);
                } else if (dispatch_result.auth_version > 0) {
                    auth::AuthSnapshot snap;
                    snap.version = dispatch_result.auth_version;
                    snap.user_id = dispatch_result.upstream.user_id;
                    snap.group_id = dispatch_result.upstream.group_id;
                    auth_cache_.insert(key_hash, std::move(snap));
                }
                break;
            }
            if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Rejected) {
                if (dispatch::is_retryable_platform_dispatch(dispatch_result)) {
                    if (++dispatch_transport_retries > 8 ||
                        std::chrono::steady_clock::now() >= dispatch_deadline) {
                        resp.status_code = 503;
                        resp.body = error_json("provider_unavailable",
                            "Platform dispatch was unavailable before the retry deadline");
                        metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                        return 0;
                    }
                    photon::thread_usleep(static_cast<uint64_t>(
                        dispatch::platform_dispatch_retry_delay_ms(dispatch_transport_retries)) * 1000);
                    continue;
                }
                if (dispatch_result.reject_code == 10) {
                    constexpr std::string_view marker = "Media operation already exists: ";
                    auto message = dispatch_result.reject_message;
                    auto position = message.find(marker);
                    if (position != std::string::npos) {
                        auto operation_id = message.substr(position + marker.size());
                        auto existing = dispatch_.media_operation(dispatch::MediaOperationRequest{
                            .api_key_hash = key_hash,
                            .operation_id = operation_id,
                            .action = "get",
                            .request_id = request_id,
                            .client_ip = std::string(req.client_ip),
                            .idempotency_key = std::string(req.idempotency_key),
                            .request_fingerprint = request_fingerprint,
                        });
                        if (existing.accepted) {
                            resp.status_code = existing.status == "pending" || existing.status == "running" ? 202 : 200;
                            resp.body = media_view_json(existing);
                            resp.headers.emplace_back("Cache-Control", "no-store");
                            if (resp.status_code == 202) resp.headers.emplace_back("Retry-After", "3");
                            const auto prefix = existing.operation_type.starts_with("images_batch")
                                ? "/v1/images/batches/"
                                : existing.operation_type.starts_with("videos_")
                                    ? "/v1/videos/" : "/v1/images/tasks/";
                            resp.headers.emplace_back("Location", prefix + existing.operation_id);
                            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                            return 0;
                        }
                    }
                    if (dispatch_result.replay_status_code > 0
                        && !dispatch_result.replay_body.empty()) {
                        resp.status_code = dispatch_result.replay_status_code;
                        resp.content_type = dispatch_result.replay_content_type.empty()
                            ? "application/json" : dispatch_result.replay_content_type;
                        resp.body = dispatch_result.replay_body;
                        resp.stream = false;
                        resp.headers.emplace_back("Cache-Control", "no-store");
                        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                        return 0;
                    }
                }
                resp.status_code = dispatch_result.reject_code <= 1 ? 401
                    : dispatch_result.reject_code == 2 ? 402
                    : dispatch_result.reject_code == 3 || dispatch_result.reject_code == 5
                        || dispatch_result.reject_code == 7 ? 429
                    : dispatch_result.reject_code == 8 ? 409
                    : dispatch_result.reject_code == 10 ? 409
                    : dispatch_result.reject_code == 9 ? 403
                    : dispatch_result.reject_code == 11 ? 503
                    : dispatch_result.reject_code == 13 ? 400
                    : 503;
                const auto reject_type = dispatch_result.reject_code <= 1
                    ? "authentication_error"
                    : dispatch_result.reject_code == 2 ? "insufficient_quota"
                    : dispatch_result.reject_code == 3 || dispatch_result.reject_code == 5
                        || dispatch_result.reject_code == 7 ? "rate_limit_error"
                    : dispatch_result.reject_code == 8 ? "idempotency_conflict"
                    : dispatch_result.reject_code == 9 ? "permission_error"
                    : dispatch_result.reject_code == 10 ? "idempotency_replay"
                    : dispatch_result.reject_code == 11 ? "pricing_unavailable"
                    : dispatch_result.reject_code == 13 ? "content_policy_violation"
                    : "provider_unavailable";
                resp.body = error_json(reject_type, dispatch_result.reject_message);
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                return 0;
            }
            if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Wait) {
                if (++dispatch_waits > 8 || std::chrono::steady_clock::now() >= dispatch_deadline) {
                    resp.status_code = 503;
                    resp.body = R"({"error":{"type":"provider_unavailable","message":"No upstream Provider account became available before the deadline"}})";
                    metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                    metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
                    return 0;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    dispatch_deadline - std::chrono::steady_clock::now()).count();
                auto wait_ms = std::min<int64_t>(
                    std::max(dispatch_result.wait_timeout_ms, 1), std::min<int64_t>(1000, remaining));
                photon::thread_usleep(
                    static_cast<uint64_t>(wait_ms) * 1000);
                continue;
            }
        }

        // Forward
        platform::FaultInjection::crash_if_configured(
            "gateway.before_provider_dispatch", request_id);
        auto& target = dispatch_result.upstream;

        const auto upstream_format = format_from_name(target.upstream_format,
            target.platform == "anthropic" || target.platform == "claude"
                ? protocol::Format::Anthropic
                : target.platform == "gemini" || target.platform == "google"
                    ? protocol::Format::Gemini
                    : target.platform == "grok" || target.platform == "xai"
                        ? protocol::Format::OpenAIChatCompletions
                        : target.upstream_path == "/v1/responses"
                            ? protocol::Format::OpenAIResponses
                            : protocol::Format::OpenAIChatCompletions);
        last_upstream_format = upstream_format;

        std::string upstream_body;
        if (req.body.empty() || !chat_capability(spec.capability)) {
            upstream_body = req.body;
        } else {
            auto conversion = protocol::Converter::convert_request(req.body, inbound_format, upstream_format, target.mapped_model);
            if (!conversion.success) {
                forward_result.status_code = 400;
                forward_result.content_type = "application/json";
                forward_result.body = R"({"error":{"type":"invalid_request_error","message":")" + conversion.error + R"("}})";
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                terminal_abort = true;
                break;
            }
            upstream_body = std::move(conversion.body);
        }

        const auto stream_mode = is_stream && inbound_format != upstream_format
            ? forwarder::ProtocolMode::CrossProtocol
            : forwarder::ProtocolMode::Passthrough;

        collector_.record_evidence(
            dispatch_result.lease_token, "forwarded",
            "gateway", "Provider transport authorized");
        const auto forwarded_ack = dispatch_.record_lease_evidence(
            dispatch_result.lease_token, dispatch::LeaseEvidenceStage::Forwarded,
            "Provider transport authorized");
        if (forwarded_ack.acknowledged()) {
            collector_.acknowledge_evidence(
                dispatch_result.lease_token, "forwarded");
        }
        if (!forwarded_ack.acknowledged()) {
            const auto abort_ack = dispatch_.abort(dispatch_result.lease_token,
                "forward_evidence_unavailable");
            if (!abort_ack.acknowledged()) {
                LOG_ERROR("Safe abort after forwarding evidence failure failed for request {} lease {}: {}",
                          request_id, dispatch_result.lease_token, abort_ack.error_code);
                platform::FaultInjection::crash_if_configured(
                    "gateway.forward_evidence_abort_failed", request_id);
            }
            forward_result.status_code = 503;
            forward_result.content_type = "application/json";
            forward_result.body = R"({"error":{"type":"platform_unavailable","message":"Unable to persist Provider dispatch evidence"}})";
            terminal_abort = true;
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        forwarder::ForwardRequest forward_request{
            .method = req.method,
            .body = upstream_body,
            .content_type = req.content_type,
            .accept = req.accept,
            .user_agent = req.user_agent,
            .request_id = request_id,
            .idempotency_key = req.idempotency_key,
            .headers = req.headers,
            .stream = is_stream,
            .stream_source = upstream_format,
            .stream_target = inbound_format,
            .stream_write = resp.stream_write,
            .stream_policy = is_stream && chat_capability(spec.capability)
                ? forwarder::StreamPolicyFn([&](std::string_view content) {
                    const auto policy = dispatch_.evaluate_response_content(
                        dispatch_result.lease_token, std::string(content),
                        std::string(spec.name));
                    switch (dispatch::content_policy_disposition(policy)) {
                    case dispatch::ContentPolicyDisposition::Allow:
                        return forwarder::StreamPolicyDecision::Allowed();
                    case dispatch::ContentPolicyDisposition::Block:
                        return forwarder::StreamPolicyDecision::Blocked(
                            policy.error_code.empty()
                                ? "content_policy_blocked" : policy.error_code,
                            "Provider response was withheld by the active content policy");
                    case dispatch::ContentPolicyDisposition::FailClosed:
                        return forwarder::StreamPolicyDecision::FailedClosed(
                            policy.error_code.empty()
                                ? "content_policy_unavailable" : policy.error_code,
                            "Provider response could not be cleared for delivery");
                    }
                    return forwarder::StreamPolicyDecision::FailedClosed(
                        "content_policy_unavailable",
                        "Provider response could not be cleared for delivery");
                })
                : forwarder::StreamPolicyFn{},
            .response_start = [&](int status, std::string_view content_type,
                                  const std::vector<std::pair<std::string, std::string>>& headers) {
                resp.status_code = status;
                resp.content_type = content_type.empty() ? "application/octet-stream" : std::string(content_type);
                resp.headers = headers;
                const auto has_request_id = std::any_of(resp.headers.begin(), resp.headers.end(),
                    [](const auto& header) {
                        return header.first == "X-Request-ID" || header.first == "x-request-id";
                    });
                if (!has_request_id) resp.headers.emplace_back("X-Request-ID", request_id);
                resp.stream = is_stream;
            },
            .output_started = [&] {
                collector_.record_evidence(
                    dispatch_result.lease_token, "output_started",
                    "gateway", "first response bytes written to client");
                const auto ack = dispatch_.record_lease_evidence(
                    dispatch_result.lease_token, dispatch::LeaseEvidenceStage::OutputStarted,
                    "first response bytes written to client");
                if (ack.acknowledged()) {
                    collector_.acknowledge_evidence(
                        dispatch_result.lease_token, "output_started");
                } else {
                    LOG_WARN("Output evidence RPC failed, retained for retry: request={} lease={} error={}",
                             request_id, dispatch_result.lease_token, ack.error_code);
                }
                platform::FaultInjection::crash_if_configured(
                    "gateway.after_output_started", request_id);
            },
            .client_disconnected = req.client_disconnected,
        };
        forward_result = forwarder_->forward(target, forward_request, stream_mode);
        platform::FaultInjection::crash_if_configured(
            "gateway.after_provider_completion", request_id);
        const auto malformed_provider_usage = forward_result.malformed_usage;

        // A client cancellation is not evidence that the Provider was never
        // charged.  Once forwarding evidence exists, retain the hold unless a
        // complete stream was observed; never retry a request after output or
        // an ambiguous partial stream.
        if ((forward_result.client_disconnect && !forward_result.output_started)
            || forward_result.stream_incomplete) {
            // Use a distinct abort reason when the incomplete stream was caused
            // by a mid-stream content policy boundary so the Platform lease
            // settlement can differentiate policy enforcement from transport
            // failures.  The disposition stays Unknown because earlier events
            // were already delivered to the client.
            const auto stream_abort_reason =
                forward_result.policy_blocked ? "response_content_policy_blocked"
                : forward_result.policy_failed_closed ? "response_content_policy_fail_closed"
                : forward_result.client_disconnect ? "client_disconnect"
                : "incomplete_provider_stream";
            platform::FaultInjection::crash_if_configured(
                "gateway.during_cancellation", request_id);
            auto abort_ack = dispatch_.abort(
                dispatch_result.lease_token,
                stream_abort_reason,
                dispatch::LeaseAbortDisposition::Unknown,
                forward_result.provider_status_code);
            if (!abort_ack.acknowledged()) {
                LOG_ERROR("Ambiguous streaming abort failed for request {} lease {}: {}",
                          request_id, dispatch_result.lease_token, abort_ack.error_code);
            }
            terminal_abort = true;
            metrics.upstream_errors.fetch_add(1, std::memory_order_relaxed);
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            if (forward_result.stream_incomplete && !forward_result.output_started) {
                resp.content_type = "application/json";
            }
        }

        if (malformed_provider_usage) {
            dispatch_.report_upstream_error(dispatch::ErrorReportData{
                .account_id = target.account_id,
                .status_code = 502,
                .retry_after_ms = 0,
                .request_id = request_id,
            });
            const auto abort_ack = dispatch_.abort(
                dispatch_result.lease_token, "malformed_provider_usage",
                dispatch::LeaseAbortDisposition::Unknown,
                forward_result.provider_status_code);
            if (!abort_ack.acknowledged()) {
                LOG_ERROR("Malformed usage abort failed for request {} lease {}: {}",
                          request_id, dispatch_result.lease_token, abort_ack.error_code);
            }
            terminal_abort = true;
            forward_result.status_code = 502;
            metrics.upstream_errors.fetch_add(1, std::memory_order_relaxed);
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        }

        // Check if we need failover
        if (forward_result.status_code >= 400 && !malformed_provider_usage
            && !terminal_abort) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            const auto explicit_provider_rejection =
                forwarder::is_explicit_provider_rejection(forward_result);
            auto retryable = explicit_provider_rejection && spec.can_failover
                && !forward_result.output_started
                && elapsed_ms < retry_policy.max_elapsed_ms
                && retry_policy.is_retryable_status(forward_result.status_code);
            auto action = retryable
                ? failover.handle_error(target.account_id, forward_result.status_code)
                : forwarder::FailoverController::Action::Exhausted;

            dispatch::ErrorReportData err{
                .account_id = target.account_id,
                .status_code = forward_result.status_code,
                .retry_after_ms = forward_result.retry_after_ms,
                .request_id = request_id,
            };
            if (retryable) dispatch_.report_upstream_error(err);
            auto abort_ack = dispatch_.abort(dispatch_result.lease_token, "upstream_failure",
                explicit_provider_rejection ? dispatch::LeaseAbortDisposition::NoCharge
                                            : dispatch::LeaseAbortDisposition::Unknown,
                forward_result.provider_status_code);
            if (!abort_ack.acknowledged()) {
                LOG_ERROR("Abort failed for request {} lease {}: {}",
                          request_id, dispatch_result.lease_token, abort_ack.error_code);
            }
            metrics.upstream_errors.fetch_add(1, std::memory_order_relaxed);

            if (action == forwarder::FailoverController::Action::SwitchAccount ||
                action == forwarder::FailoverController::Action::Continue) {
                // The failed lease is terminal. Keep the external idempotency
                // key stable, but give each internal retry a unique lease ID.
                dispatch_req.request_id = request_id + ":retry:" +
                    std::to_string(++dispatch_retry_sequence);
                dispatch_req.excluded_accounts.assign(
                    failover.failed_accounts().begin(),
                    failover.failed_accounts().end());
                metrics.failovers.fetch_add(1, std::memory_order_relaxed);
                photon::thread_usleep(
                    static_cast<uint64_t>(retry_policy.compute_delay(failover.switch_count())) * 1000);
                continue;
            }
            terminal_abort = true;
        }

        break;
    }

    bool response_policy_overridden = false;
    if ((!is_stream || forward_result.status_code >= 400) && !forward_result.body.empty()) {
        resp.body = std::move(forward_result.body);
        const bool explicit_provider_error = forward_result.provider_response_received
            && forward_result.provider_status_code >= 400;
        // Forwarder-generated transport/protocol bodies use the OpenAI-shaped
        // internal error contract even when the selected Provider speaks
        // Anthropic or Gemini. Only an explicit Provider error is native to
        // last_upstream_format and eligible for same-format passthrough.
        const auto error_source = explicit_provider_error
            ? last_upstream_format : protocol::Format::OpenAIChatCompletions;
        if (forward_result.status_code >= 400 && inbound_format != error_source) {
            const auto error_status = explicit_provider_error
                ? forward_result.provider_status_code : forward_result.status_code;
            resp.body = protocol::Converter::convert_error(
                resp.body, error_status, error_source, inbound_format);
            resp.content_type = "application/json";
        }
        if (forward_result.status_code < 400 && chat_capability(spec.capability)
            && inbound_format != last_upstream_format) {
            auto converted = protocol::Converter::convert_response_checked(
                resp.body, last_upstream_format, inbound_format, parsed.model);
            if (converted.success) {
                resp.body = std::move(converted.body);
            } else {
                auto abort_ack = dispatch_.abort(dispatch_result.lease_token,
                    "response_conversion_failed", dispatch::LeaseAbortDisposition::Unknown,
                    forward_result.provider_status_code);
                if (!abort_ack.acknowledged())
                    LOG_ERROR("Conversion abort failed for request {}: {}",
                        request_id, abort_ack.error_code);
                terminal_abort = true;
                forward_result.status_code = 502;
                resp.status_code = 502;
                resp.body = error_json("response_conversion_error", converted.error);
                metrics.conversion_failures.fetch_add(1, std::memory_order_relaxed);
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (forward_result.status_code >= 200 && forward_result.status_code < 400
            && spec.capability == Capability::Embeddings) {
            auto validation = protocol::Converter::validate_embeddings_response(
                req.body, resp.body);
            if (!validation.valid) {
                auto abort_ack = dispatch_.abort(dispatch_result.lease_token,
                    "embeddings_response_invalid", dispatch::LeaseAbortDisposition::Unknown,
                    forward_result.provider_status_code);
                if (!abort_ack.acknowledged())
                    LOG_ERROR("Embeddings response abort failed for request {}: {}",
                        request_id, abort_ack.error_code);
                terminal_abort = true;
                forward_result.status_code = 502;
                resp.status_code = 502;
                resp.content_type = "application/json";
                resp.body = error_json("provider_protocol_error", validation.message);
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (forward_result.status_code >= 200 && forward_result.status_code < 400
            && (spec.capability == Capability::Models || spec.capability == Capability::GeminiModels
                || spec.capability == Capability::CountTokens)) {
            const auto validation = spec.capability == Capability::CountTokens
                ? protocol::Converter::validate_count_tokens_response(resp.body)
                : protocol::Converter::validate_models_response(resp.body, spec.inbound_format);
            if (!validation.valid) {
                const auto abort_ack = dispatch_.abort(
                    dispatch_result.lease_token, "catalog_response_invalid",
                    dispatch::LeaseAbortDisposition::Unknown,
                    forward_result.provider_status_code);
                if (!abort_ack.acknowledged())
                    LOG_ERROR("Catalog response abort failed for request {} lease {}: {}",
                        request_id, dispatch_result.lease_token, abort_ack.error_code);
                terminal_abort = true;
                forward_result.status_code = 502;
                resp.status_code = 502;
                resp.content_type = "application/json";
                resp.body = error_json("provider_protocol_error", validation.message);
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (forward_result.status_code >= 200 && forward_result.status_code < 400
            && !is_stream && spec.capability == Capability::Responses) {
            const auto validation = protocol::Converter::validate_responses_response(resp.body);
            if (!validation.valid) {
                const auto abort_ack = dispatch_.abort(
                    dispatch_result.lease_token, "responses_response_invalid",
                    dispatch::LeaseAbortDisposition::Unknown,
                    forward_result.provider_status_code);
                if (!abort_ack.acknowledged())
                    LOG_ERROR("Responses response abort failed for request {} lease {}: {}",
                        request_id, dispatch_result.lease_token, abort_ack.error_code);
                terminal_abort = true;
                forward_result.status_code = 502;
                resp.status_code = 502;
                resp.content_type = "application/json";
                resp.body = error_json("provider_protocol_error", validation.message);
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (!is_stream && !terminal_abort && !forward_result.malformed_usage
        && forward_result.status_code >= 200 && forward_result.status_code < 400
        && chat_capability(spec.capability) && !resp.body.empty()) {
        const auto policy = dispatch_.evaluate_response_content(
            dispatch_result.lease_token, resp.body, std::string(spec.name));
        switch (dispatch::content_policy_disposition(policy)) {
            case dispatch::ContentPolicyDisposition::Allow:
                break;
            case dispatch::ContentPolicyDisposition::Block:
                response_policy_overridden = true;
                resp.status_code = 400;
                resp.content_type = "application/json";
                resp.body = error_json("content_policy_violation",
                    "Provider response was withheld by the active content policy");
                resp.headers.emplace_back("Cache-Control", "no-store");
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                break;
            case dispatch::ContentPolicyDisposition::FailClosed:
                response_policy_overridden = true;
                resp.status_code = 503;
                resp.content_type = "application/json";
                resp.body = error_json("content_policy_unavailable",
                    "Provider response could not be cleared for delivery");
                resp.headers.emplace_back("Cache-Control", "no-store");
                metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR("Response content policy failed closed for request {} lease {}: {}",
                    request_id, dispatch_result.lease_token, policy.error_code);
                break;
        }
    }

    if (forward_result.malformed_usage) {
        resp.status_code = 502;
        resp.content_type = "application/json";
        resp.body = error_json("provider_error", "Provider returned malformed usage");
    }

    bool defer_media_usage = false;
    bool media_response_overridden = false;
    if (persistent_create && !dispatch_result.upstream.media_operation_id.empty()) {
        media_response_overridden = true;
        const auto task_id = provider_task_id(resp.body);
        auto media_status = terminal_abort || forward_result.status_code >= 400
            ? std::string("failed") : provider_media_status(resp.body, task_id);
        const auto metadata = resp.body.size() <= 512 * 1024 ? resp.body : std::string{};
        auto operation = dispatch_.media_operation(dispatch::MediaOperationRequest{
            .api_key_hash = key_hash,
            .operation_id = dispatch_result.upstream.media_operation_id,
            .action = media_status == "failed" ? "fail" : "attach",
            .request_id = request_id,
            .client_ip = std::string(req.client_ip),
            .idempotency_key = std::string(req.idempotency_key),
            .request_fingerprint = request_fingerprint,
            .status = media_status,
            .upstream_task_id = task_id,
            .output_metadata = metadata,
            .output_url = provider_output_url(resp.body),
            .content_type = forward_result.content_type,
            .progress = media_status == "running" ? 0 : 100,
        });
        if (!operation.accepted) {
            dispatch_.abort(dispatch_result.lease_token, "media_operation_persist_failed",
                dispatch::LeaseAbortDisposition::Unknown,
                forward_result.provider_status_code);
            resp.status_code = operation.status_code > 0 ? operation.status_code : 503;
            resp.body = R"({"error":{"type":"media_operation_error","message":"Unable to persist media operation state"}})";
            terminal_abort = true;
        } else {
            defer_media_usage = media_status == "running";
            resp.status_code = defer_media_usage ? 202 : 200;
            resp.body = media_view_json(operation);
            resp.headers.emplace_back("Cache-Control", "no-store");
            if (defer_media_usage) resp.headers.emplace_back("Retry-After", "3");
            const std::string poll_prefix = spec.capability == Capability::ImagesAsync
                ? "/v1/images/tasks/"
                : spec.capability == Capability::ImagesBatch
                    ? "/v1/images/batches/" : "/v1/videos/";
            resp.headers.emplace_back("Location", poll_prefix + operation.operation_id);
        }
    }

    // Catalog and token-count calls reserve an account lease for routing and
    // provider isolation, but they do not represent billable generation.
    // Release their hold through the normal no-charge terminal path instead of
    // enqueuing a zero-token usage event that cannot carry a price snapshot.
    const bool non_billable_control = spec.capability == Capability::Models
        || spec.capability == Capability::GeminiModels
        || spec.capability == Capability::CountTokens
        || spec.capability == Capability::ResponsesSubpath;
    if (non_billable_control && !terminal_abort
        && forward_result.status_code >= 200 && forward_result.status_code < 400) {
        const auto abort_ack = dispatch_.abort(
            dispatch_result.lease_token, "non_billable_control",
            dispatch::LeaseAbortDisposition::NoCharge,
            forward_result.provider_status_code);
        if (!abort_ack.acknowledged()) {
            LOG_ERROR("Control request release failed for {} lease {}: {}",
                request_id, dispatch_result.lease_token, abort_ack.error_code);
        } else {
            terminal_abort = true;
        }
    }

    // --- Step 8: Report usage (fire-and-forget) ---
    const bool persist_response = !is_stream && !terminal_abort && !defer_media_usage
        && !media_response_overridden && forward_result.status_code >= 200
        && forward_result.status_code < 400 && resp.body.size() <= 4 * 1024 * 1024;
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto& upstream = dispatch_result.upstream;
    const auto media_response_usage = protocol::Converter::parse_media_response(
        resp.body, matched.operation);
    // A truncated Provider stream remains ambiguous unless the Provider sent
    // a valid usage object before the connection ended. In that case retain
    // the normal settlement event in the durable outbox after the unknown
    // abort above; Platform will settle the reconciliation-needed lease
    // exactly once and replay it safely after a Gateway restart.
    const bool late_usage_candidate = is_stream && forward_result.stream_incomplete
        && !forward_result.malformed_usage
        && !forward_result.provider_usage_json.empty()
        && (forward_result.input_tokens > 0 || forward_result.output_tokens > 0
            || forward_result.cache_create_tokens > 0 || forward_result.cache_read_tokens > 0
            || forward_result.reasoning_tokens > 0);
    usage::UsageEvent event{
        .lease_token = dispatch_result.lease_token,
        .request_id = request_id,
        .api_key_id = dispatch_result.api_key_id,
        .user_id = upstream.user_id,
        .account_id = upstream.account_id,
        .group_id = upstream.group_id,
        .model = parsed.model,
        .upstream_model = upstream.mapped_model,
        .input_tokens = forward_result.input_tokens,
        .output_tokens = forward_result.output_tokens,
        .cache_create_tokens = forward_result.cache_create_tokens,
        .cache_read_tokens = forward_result.cache_read_tokens,
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
        .first_token_ms = forward_result.first_token_ms,
        .stream = is_stream,
        .client_disconnect = forward_result.client_disconnect,
        .status_code = forward_result.status_code,
        .input_image_count = media_request_usage.input_image_count,
        .output_image_count = media_response_usage.output_image_count > 0
            ? media_response_usage.output_image_count : media_request_usage.output_image_count,
        .image_size = !media_response_usage.image_size.empty()
            ? media_response_usage.image_size : media_request_usage.image_size,
        .video_count = media_response_usage.video_count > 0
            ? media_response_usage.video_count : media_request_usage.video_count,
        .video_resolution = !media_response_usage.video_resolution.empty()
            ? media_response_usage.video_resolution : media_request_usage.video_resolution,
        .video_duration_seconds = media_response_usage.video_duration_seconds > 0
            ? media_response_usage.video_duration_seconds
            : media_request_usage.video_duration_seconds,
        .disconnect_reason = forward_result.disconnect_reason,
        .provider_usage_json = forward_result.provider_usage_json,
        .reasoning_tokens = forward_result.reasoning_tokens,
        .service_tier = forward_result.service_tier,
        .upstream_endpoint = dispatch_result.upstream.upstream_path,
        .cancellation_reason = forward_result.cancellation_reason,
        .media_operation_id = dispatch_result.upstream.media_operation_id,
        .pricing_version = "v1",
        .response_status_code = persist_response ? resp.status_code : 0,
        .response_content_type = persist_response ? resp.content_type : "",
        .response_body = persist_response ? resp.body : "",
    };
    if ((!terminal_abort || late_usage_candidate) && !defer_media_usage) {
        try {
            collector_.record(event);
            metrics.usage_events_buffered.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            LOG_ERROR("Durable usage outbox write failed for request {}: {}", request_id, ex.what());
            dispatch::UsageReportData fallback{
                .lease_token = event.lease_token,
                .request_id = event.request_id,
                .api_key_id = event.api_key_id,
                .user_id = event.user_id,
                .account_id = event.account_id,
                .group_id = event.group_id,
                .model = event.model,
                .upstream_model = event.upstream_model,
                .input_tokens = event.input_tokens,
                .output_tokens = event.output_tokens,
                .cache_create_tokens = event.cache_create_tokens,
                .cache_read_tokens = event.cache_read_tokens,
                .duration_ms = event.duration_ms,
                .first_token_ms = event.first_token_ms,
                .stream = event.stream,
                .client_disconnect = event.client_disconnect,
                .status_code = event.status_code,
                .input_image_count = event.input_image_count,
                .output_image_count = event.output_image_count,
                .image_size = event.image_size,
                .video_count = event.video_count,
                .video_resolution = event.video_resolution,
                .video_duration_seconds = event.video_duration_seconds,
                .realtime_duration_ms = event.realtime_duration_ms,
                .realtime_frames = event.realtime_frames,
                .disconnect_reason = event.disconnect_reason,
                .provider_usage_json = event.provider_usage_json,
                .reasoning_tokens = event.reasoning_tokens,
                .service_tier = event.service_tier,
                .upstream_endpoint = event.upstream_endpoint,
                .cancellation_reason = event.cancellation_reason,
                .media_operation_id = event.media_operation_id,
                .pricing_version = event.pricing_version,
                .response_status_code = event.response_status_code,
                .response_content_type = event.response_content_type,
                .response_body = event.response_body,
            };
            auto ack = dispatch_.report_usage(fallback);
            if (!ack.acknowledged()) {
                metrics.usage_report_failures.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR("Synchronous usage fallback failed for lease {}: {}",
                          event.lease_token, ack.error_code);
            }
        }
    }

    if (!media_response_overridden && !response_policy_overridden)
        resp.status_code = forward_result.status_code > 0 ? forward_result.status_code : 200;
    metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
    return 0;
}

std::string GatewayHandler::extract_api_key(const HttpRequest& req) {
    if (req.authorization.starts_with("Bearer ")) {
        return std::string(req.authorization.substr(7));
    }
    if (!req.x_api_key.empty()) {
        return std::string(req.x_api_key);
    }
    return "";
}

std::string GatewayHandler::compute_session_hash(std::string_view key_hash,
                                                  std::string_view metadata_user_id,
                                                  std::string_view body,
                                                  std::string_view model) {
    std::string context;
    context.reserve(key_hash.size() + model.size() + metadata_user_id.size() + 4098);
    context.append(key_hash);
    context.push_back('\n');
    context.append(model);
    context.push_back('\n');
    if (!metadata_user_id.empty()) {
        context.append(metadata_user_id);
    } else {
        auto context_size = std::min<size_t>(body.size(), 4096);
        context.append(body.data(), context_size);
    }
    auto hash = XXH64(context.data(), context.size(), 0);
    return std::format("{:016x}", hash);
}

}  // namespace gateway::server
