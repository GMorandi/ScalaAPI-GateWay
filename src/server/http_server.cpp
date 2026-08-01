#include "server/http_server.h"
#include "server/router.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "usage/usage_reporter.h"
#include "platform/logging.h"

namespace gateway::server {

struct HttpServer::Impl {
    HttpServerConfig config;
    std::unique_ptr<Router> router;
};

std::unique_ptr<HttpServer> HttpServer::create(
    const HttpServerConfig& config,
    cache::GarnetClient& garnet,
    dispatch::CapnpDispatchClient& dispatch,
    usage::UsageReporter& usage_reporter) {

    auto srv = std::make_unique<HttpServer>();
    srv->impl_ = std::make_unique<Impl>();
    srv->impl_->config = config;
    srv->impl_->router = Router::create(garnet, dispatch, usage_reporter);

    LOG_INFO("HTTP server created on port {} (core {})",
             config.port, config.core_id);
    return srv;
}

HttpServer::~HttpServer() = default;

}  // namespace gateway::server
