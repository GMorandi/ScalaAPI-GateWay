#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gateway::cache { class GarnetClient; }
namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::usage { class UsageReporter; }

namespace gateway::server {

struct HttpServerConfig {
    uint16_t port = 8080;
    int core_id = 0;
    size_t max_body_size = 32 * 1024 * 1024;
};

class HttpServer {
public:
    static std::unique_ptr<HttpServer> create(
        const HttpServerConfig& config,
        cache::GarnetClient& garnet,
        dispatch::CapnpDispatchClient& dispatch,
        usage::UsageReporter& usage_reporter);

    ~HttpServer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::server
