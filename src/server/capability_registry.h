#pragma once

#include "dispatch/capnp_dispatch_client.h"
#include "protocol/converter.h"

#include <string_view>

namespace gateway::server {

enum class Capability {
    Messages,
    ChatCompletions,
    Responses,
    ResponsesSubpath,
    CountTokens,
    Models,
    Search,
    Embeddings,
    ImagesSync,
    ImagesAsync,
    ImagesBatch,
    Videos,
    Realtime,
    GeminiModels,
    GeminiGenerate,
    Antigravity,
    AudioTts,
    AudioStt,
};

struct CapabilitySpec {
    Capability capability;
    std::string_view name;
    dispatch::DispatchRequest::EndpointKind endpoint;
    protocol::Format inbound_format;
    bool can_stream;
    bool can_failover;
    bool requires_persistent_task;
    bool realtime;
};

struct MatchedCapability {
    const CapabilitySpec* spec = nullptr;
    std::string_view operation;
    std::string_view force_platform;
};

// Returns a route only when both its path and method are part of the public
// compatibility contract.  User-controlled path components are validated
// before a route is allowed to reach dispatch.
MatchedCapability match_capability(std::string_view method, std::string_view path);

bool is_safe_path_suffix(std::string_view suffix);
bool is_safe_gemini_model(std::string_view model);
bool is_safe_query_string(std::string_view query);

}  // namespace gateway::server
