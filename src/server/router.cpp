#include "server/router.h"
#include "server/gateway_handler.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "platform/logging.h"

namespace gateway::server {

struct Router::Impl {
    auth::SpeculativeCache* auth_cache;
    usage::UsageCollector* collector;
    std::unique_ptr<GatewayHandler> gateway;
};

std::unique_ptr<Router> Router::create(
    cache::GarnetClient& garnet,
    dispatch::CapnpDispatchClient& dispatch,
    auth::SpeculativeCache& auth_cache,
    usage::UsageCollector& collector) {

    auto r = std::make_unique<Router>();
    r->impl_ = std::make_unique<Impl>();
    r->impl_->auth_cache = &auth_cache;
    r->impl_->collector = &collector;
    r->impl_->gateway = std::make_unique<GatewayHandler>(
        garnet, dispatch, collector, auth_cache);
    return r;
}

Router::~Router() = default;

int Router::handle_request(const HttpRequest& req, HttpResponse& resp) {
    auto path = req.path;

    if (path == "/v1/messages" || path == "/messages") {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Messages);
    }

    if (path == "/v1/chat/completions" || path == "/chat/completions") {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::ChatCompletions);
    }

    if (path == "/v1/responses" || path == "/responses") {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Responses);
    }

    if (path == "/v1/models" || path == "/models") {
        resp.status_code = 200;
        return 0;
    }

    if (path.starts_with("/v1beta/")) {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Gemini);
    }

    resp.status_code = 404;
    return 0;
}

}  // namespace gateway::server
