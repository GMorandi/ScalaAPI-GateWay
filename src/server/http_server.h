#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gateway::cache { class GarnetClient; }
namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::auth { class SpeculativeCache; }
namespace gateway::usage { class UsageCollector; }

namespace gateway::server {

struct HttpServerConfig {
    uint16_t port = 8080;
    int core_id = 0;
    size_t max_body_size = 32 * 1024 * 1024;
    std::string trusted_proxy_cidrs;
    uint32_t stream_timeout_ms = 300'000;
};

class HttpServer {
public:
    static std::unique_ptr<HttpServer> create(
        const HttpServerConfig& config,
        cache::GarnetClient& garnet,
        dispatch::CapnpDispatchClient& dispatch,
        auth::SpeculativeCache& auth_cache,
        usage::UsageCollector& collector);

    ~HttpServer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::server
