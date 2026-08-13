#include "server/capability_registry.h"

#include <array>

namespace gateway::server {
namespace {

using Endpoint = dispatch::DispatchRequest::EndpointKind;
using Format = protocol::Format;

constexpr CapabilitySpec kMessages{
    Capability::Messages, "messages", Endpoint::Messages, Format::Anthropic, true, true, false, false};
constexpr CapabilitySpec kChat{
    Capability::ChatCompletions, "chat_completions", Endpoint::ChatCompletions, Format::OpenAIChatCompletions, true, true, false, false};
constexpr CapabilitySpec kResponses{
    Capability::Responses, "responses", Endpoint::Responses, Format::OpenAIResponses, true, true, false, false};
constexpr CapabilitySpec kResponsesSubpath{
    Capability::ResponsesSubpath, "responses_subpath", Endpoint::Responses, Format::OpenAIResponses, true, true, false, false};
constexpr CapabilitySpec kCountTokens{
    Capability::CountTokens, "count_tokens", Endpoint::CountTokens, Format::Anthropic, false, true, false, false};
constexpr CapabilitySpec kModels{
    Capability::Models, "models", Endpoint::Models, Format::OpenAIChatCompletions, false, false, false, false};
constexpr CapabilitySpec kSearch{
    Capability::Search, "search", Endpoint::AlphaSearch, Format::OpenAIResponses, true, true, false, false};
constexpr CapabilitySpec kEmbeddings{
    Capability::Embeddings, "embeddings", Endpoint::Embeddings, Format::OpenAIChatCompletions, false, true, false, false};
constexpr CapabilitySpec kImagesSync{
    Capability::ImagesSync, "images_sync", Endpoint::Images, Format::OpenAIChatCompletions, false, true, false, false};
constexpr CapabilitySpec kImagesAsync{
    Capability::ImagesAsync, "images_async", Endpoint::Images, Format::OpenAIChatCompletions, false, false, true, false};
constexpr CapabilitySpec kImagesBatch{
    Capability::ImagesBatch, "images_batch", Endpoint::Images, Format::OpenAIChatCompletions, false, false, true, false};
constexpr CapabilitySpec kVideos{
    Capability::Videos, "videos", Endpoint::Videos, Format::OpenAIChatCompletions, false, false, true, false};
constexpr CapabilitySpec kRealtime{
    Capability::Realtime, "realtime", Endpoint::Realtime, Format::OpenAIResponses, true, false, false, true};
constexpr CapabilitySpec kGeminiModels{
    Capability::GeminiModels, "gemini_models", Endpoint::Models, Format::Gemini, false, false, false, false};
constexpr CapabilitySpec kGeminiGenerate{
    Capability::GeminiGenerate, "gemini_generate", Endpoint::Gemini, Format::Gemini, true, true, false, false};
constexpr CapabilitySpec kAntigravity{
    Capability::Antigravity, "antigravity", Endpoint::Antigravity, Format::Anthropic, true, true, false, false};

bool eq_any(std::string_view value, std::initializer_list<std::string_view> values) {
    for (auto candidate : values) {
        if (value == candidate) return true;
    }
    return false;
}

bool safe_segment(std::string_view segment) {
    if (segment.empty() || segment.size() > 128) return false;
    bool dots_only = true;
    for (char c : segment) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!allowed) return false;
        if (c != '.') dots_only = false;
    }
    return !dots_only;
}

MatchedCapability match_batch_path(std::string_view method, std::string_view relative,
                                   std::string_view force_platform) {
    constexpr std::string_view prefix = "/images/batches/";
    if (!relative.starts_with(prefix)) return {};
    auto suffix = relative.substr(prefix.size());
    const auto slash = suffix.find('/');
    const auto batch_id = suffix.substr(0, slash);
    if (!safe_segment(batch_id)) return {};
    if (slash == std::string_view::npos)
        return eq_any(method, {"GET", "DELETE"})
            ? MatchedCapability{&kImagesBatch, method == "GET" ? "images_batch_get" : "images_batch_delete", force_platform}
            : MatchedCapability{};
    const auto tail = suffix.substr(slash + 1);
    if (tail == "items" && method == "GET")
        return {&kImagesBatch, "images_batch_items", force_platform};
    if (tail == "download" && method == "GET")
        return {&kImagesBatch, "images_batch_download", force_platform};
    if (tail == "cancel" && method == "POST")
        return {&kImagesBatch, "images_batch_cancel", force_platform};
    if (tail == "outputs" && method == "DELETE")
        return {&kImagesBatch, "images_batch_delete_outputs", force_platform};
    constexpr std::string_view content_prefix = "items/";
    if (tail.starts_with(content_prefix) && tail.ends_with("/content") && method == "GET") {
        auto custom_id = tail.substr(content_prefix.size(),
            tail.size() - content_prefix.size() - std::string_view("/content").size());
        if (safe_segment(custom_id)) return {&kImagesBatch, "images_batch_item_content", force_platform};
    }
    return {};
}

MatchedCapability match_video_path(std::string_view method, std::string_view relative,
                                   std::string_view force_platform) {
    constexpr std::string_view prefix = "/videos/";
    if (!relative.starts_with(prefix)) return {};
    auto suffix = relative.substr(prefix.size());
    const auto slash = suffix.find('/');
    const auto request_id = suffix.substr(0, slash);
    if (!safe_segment(request_id)) return {};
    if (slash == std::string_view::npos) {
        if (method == "GET") return {&kVideos, "videos_get", force_platform};
        if (method == "DELETE") return {&kVideos, "videos_delete", force_platform};
        return {};
    }
    const auto tail = suffix.substr(slash + 1);
    if (tail == "content" && method == "GET")
        return {&kVideos, "videos_content", force_platform};
    if (tail == "cancel" && method == "POST")
        return {&kVideos, "videos_cancel", force_platform};
    if (tail == "outputs" && method == "DELETE")
        return {&kVideos, "videos_delete_outputs", force_platform};
    return {};
}

MatchedCapability route_openai(std::string_view method, std::string_view path,
                               std::string_view prefix, std::string_view force_platform = {}) {
    const auto relative = path.substr(prefix.size());
    auto match = [&](std::string_view suffix, std::string_view verb,
                     const CapabilitySpec& spec, std::string_view operation) -> MatchedCapability {
        if (method == verb && relative == suffix) return {&spec, operation, force_platform};
        return {};
    };

    if (auto r = match("/messages", "POST", force_platform.empty() ? kMessages : kAntigravity, "messages"); r.spec) return r;
    if (auto r = match("/messages/count_tokens", "POST", kCountTokens, "count_tokens"); r.spec) return r;
    if (auto r = match("/chat/completions", "POST", kChat, "chat_completions"); r.spec) return r;
    if (auto r = match("/responses/compact", "POST", kResponses, "responses_compact"); r.spec) return r;
    if (auto r = match("/responses", "POST", kResponses, "responses"); r.spec) return r;
    if (relative == "/responses/compact") return {};
    if (relative.starts_with("/responses/") && method == "POST") {
        constexpr std::string_view cancel_suffix = "/cancel";
        const auto response_suffix = relative.substr(std::string_view("/responses/").size());
        if (response_suffix.ends_with(cancel_suffix)
            && safe_segment(response_suffix.substr(0, response_suffix.size() - cancel_suffix.size())))
            return {&kResponsesSubpath, "responses_cancel", force_platform};
    }
    if (relative.starts_with("/responses/") && method == "GET") {
        constexpr std::string_view input_items_suffix = "/input_items";
        const auto response_suffix = relative.substr(std::string_view("/responses/").size());
        if (response_suffix.ends_with(input_items_suffix)
            && safe_segment(response_suffix.substr(0, response_suffix.size() - input_items_suffix.size())))
            return {&kResponsesSubpath, "responses_input_items", force_platform};
        if (safe_segment(response_suffix))
            return {&kResponsesSubpath, "responses_get", force_platform};
    }
    if (relative.starts_with("/responses/") && method == "DELETE"
        && safe_segment(relative.substr(std::string_view("/responses/").size()))) {
        return {&kResponsesSubpath, "responses_delete", force_platform};
    }
    if (auto r = match("/responses", "GET", kRealtime, "responses_websocket"); r.spec) return r;
    if (auto r = match("/live", "POST", kRealtime, "live_create"); r.spec) return r;
    if (relative.starts_with("/live/") && method == "GET"
        && safe_segment(relative.substr(std::string_view("/live/").size())))
        return {&kRealtime, "live_sideband", force_platform};
    if (auto r = match("/alpha/search", "POST", kSearch, "alpha_search"); r.spec) return r;
    if (auto r = match("/embeddings", "POST", kEmbeddings, "embeddings"); r.spec) return r;
    if (auto r = match("/images/generations", "POST", kImagesSync, "images_generations"); r.spec) return r;
    if (auto r = match("/images/edits", "POST", kImagesSync, "images_edits"); r.spec) return r;
    if (relative == "/images/generations/async" && method == "POST")
        return {&kImagesAsync, "images_generations_async", force_platform};
    if (relative == "/images/edits/async" && method == "POST")
        return {&kImagesAsync, "images_edits_async", force_platform};
    if (relative.starts_with("/images/tasks/") && method == "GET" && safe_segment(relative.substr(14)))
        return {&kImagesAsync, "images_task_get", force_platform};
    if (relative == "/images/batches" && (method == "POST" || method == "GET"))
        return {&kImagesBatch, method == "POST" ? "images_batch_create" : "images_batch_list", force_platform};
    if (relative == "/images/batches/models" && method == "GET")
        return {&kImagesBatch, "images_batch_models", force_platform};
    if (auto r = match_batch_path(method, relative, force_platform); r.spec) return r;
    if (relative == "/videos/generations" && method == "POST")
        return {&kVideos, "videos_generations", force_platform};
    if (relative == "/videos/edits" && method == "POST")
        return {&kVideos, "videos_edits", force_platform};
    if (relative == "/videos/extensions" && method == "POST")
        return {&kVideos, "videos_extensions", force_platform};
    if (auto r = match_video_path(method, relative, force_platform); r.spec) return r;
    if (auto r = match("/models", "GET", force_platform.empty() ? kModels : kAntigravity,
                       force_platform.empty() ? "models" : "antigravity_models"); r.spec) return r;
    if (!force_platform.empty()) {
        if (auto r = match("/usage", "GET", kAntigravity, "antigravity_usage"); r.spec) return r;
    }
    return {};
}

MatchedCapability route_gemini(std::string_view method, std::string_view path,
                               std::string_view prefix, std::string_view force_platform = {}) {
    const auto relative = path.substr(prefix.size());
    if (relative == "/models" && method == "GET") return {&kGeminiModels, "gemini_models_list", force_platform};
    if (!relative.starts_with("/models/")) return {};
    auto model_action = relative.substr(8);
    const auto colon = model_action.find(':');
    const auto model = model_action.substr(0, colon);
    if (!is_safe_gemini_model(model)) return {};
    if (colon == std::string_view::npos && method == "GET")
        return {&kGeminiModels, "gemini_models_get", force_platform};
    if (colon == std::string_view::npos) return {};
    const auto action = model_action.substr(colon + 1);
    if (method == "POST" && (action == "generateContent" || action == "streamGenerateContent"))
        return {&kGeminiGenerate, action, force_platform};
    return {};
}

}  // namespace

bool is_safe_path_suffix(std::string_view suffix) {
    if (suffix.empty() || !suffix.starts_with('/')) return false;
    suffix.remove_prefix(1);
    if (suffix.empty()) return false;
    size_t segments = 0;
    while (!suffix.empty()) {
        const auto slash = suffix.find('/');
        const auto segment = suffix.substr(0, slash);
        if (!safe_segment(segment) || ++segments > 8) return false;
        if (slash == std::string_view::npos) break;
        suffix.remove_prefix(slash + 1);
    }
    return true;
}

bool is_safe_gemini_model(std::string_view model) {
    return safe_segment(model);
}

bool is_safe_query_string(std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > 4096 || query.front() != '?') return false;
    auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F');
    };
    for (size_t i = 1; i < query.size(); ++i) {
        auto c = static_cast<unsigned char>(query[i]);
        if (c < 0x21 || c > 0x7e || c == '#') return false;
        if (c != '%') continue;
        if (i + 2 >= query.size() || !hex(query[i + 1]) || !hex(query[i + 2]))
            return false;
        i += 2;
    }
    return true;
}

MatchedCapability match_capability(std::string_view method, std::string_view path) {
    if (path == "/live" || path == "/ready" || path == "/metrics") return {};
    if (path.starts_with("/antigravity/v1beta"))
        return route_gemini(method, path, "/antigravity/v1beta", "antigravity");
    if (path.starts_with("/antigravity/v1"))
        return route_openai(method, path, "/antigravity/v1", "antigravity");
    if (path == "/antigravity/models" && method == "GET")
        return {&kAntigravity, "antigravity_models", "antigravity"};
    if (path.starts_with("/v1beta")) return route_gemini(method, path, "/v1beta");
    if (path.starts_with("/backend-api/codex")) {
        if (path == "/backend-api/codex/realtime/calls" && method == "POST")
            return {&kRealtime, "codex_realtime_calls", {}};
        if (path.starts_with("/backend-api/codex/")) {
            auto sideband = path.substr(std::string_view("/backend-api/codex/").size());
            if (method == "GET" && is_safe_gemini_model(sideband)
                && sideband != "models" && sideband != "responses")
                return {&kRealtime, "codex_live_sideband", {}};
        }
        return route_openai(method, path, "/backend-api/codex");
    }
    if (path.starts_with("/v1")) return route_openai(method, path, "/v1");
    return route_openai(method, path, "");
}

}  // namespace gateway::server
