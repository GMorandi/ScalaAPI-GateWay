#include "server/router.h"
#include "server/gateway_handler.h"
#include "cache/garnet_client.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "platform/logging.h"
#include "platform/metrics.h"

#include <format>

namespace gateway::server {

struct Router::Impl {
    cache::GarnetClient* garnet;
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
    r->impl_->garnet = &garnet;
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

    if (path == "/v1/embeddings" || path == "/embeddings") {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Embeddings);
    }

    if (path == "/v1/images/generations" || path == "/images/generations") {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Images);
    }

    if (path == "/v1/messages/count_tokens" || path == "/messages/count_tokens") {
        return handle_count_tokens(req, resp);
    }

    if (path == "/v1/models" || path == "/models") {
        return handle_models(req, resp);
    }

    if (path.starts_with("/v1beta/")) {
        return impl_->gateway->handle(req, resp, dispatch::DispatchRequest::EndpointKind::Gemini);
    }

    if (path == "/metrics") {
        auto& m = platform::global_metrics();
        resp.status_code = 200;
        resp.body = std::format(
            "# HELP gateway_requests_total Total requests\n"
            "# TYPE gateway_requests_total counter\n"
            "gateway_requests_total {}\n"
            "# HELP gateway_requests_streaming Streaming requests\n"
            "# TYPE gateway_requests_streaming counter\n"
            "gateway_requests_streaming {}\n"
            "# HELP gateway_requests_failed Failed requests\n"
            "# TYPE gateway_requests_failed counter\n"
            "gateway_requests_failed {}\n"
            "# HELP gateway_dispatch_calls Dispatch RPC calls\n"
            "# TYPE gateway_dispatch_calls counter\n"
            "gateway_dispatch_calls {}\n"
            "# HELP gateway_garnet_hits Garnet cache hits\n"
            "# TYPE gateway_garnet_hits counter\n"
            "gateway_garnet_hits {}\n"
            "# HELP gateway_garnet_misses Garnet cache misses\n"
            "# TYPE gateway_garnet_misses counter\n"
            "gateway_garnet_misses {}\n"
            "# HELP gateway_upstream_errors Upstream errors\n"
            "# TYPE gateway_upstream_errors counter\n"
            "gateway_upstream_errors {}\n"
            "# HELP gateway_failovers Account failovers\n"
            "# TYPE gateway_failovers counter\n"
            "gateway_failovers {}\n"
            "# HELP gateway_active_connections Active connections\n"
            "# TYPE gateway_active_connections gauge\n"
            "gateway_active_connections {}\n",
            m.requests_total.load(), m.requests_streaming.load(),
            m.requests_failed.load(), m.dispatch_calls.load(),
            m.garnet_hits.load(), m.garnet_misses.load(),
            m.upstream_errors.load(), m.failovers.load(),
            m.active_connections.load());
        return 0;
    }

    resp.status_code = 404;
    return 0;
}

int Router::handle_models(const HttpRequest& req, HttpResponse& resp) {
    auto cached = impl_->garnet->get("models:list");
    if (cached.found && !cached.value.empty()) {
        resp.status_code = 200;
        resp.body = cached.value;
        return 0;
    }

    resp.status_code = 200;
    resp.body = R"({"object":"list","data":[]})";
    return 0;
}

int Router::handle_count_tokens(const HttpRequest& req, HttpResponse& resp) {
    int estimated = static_cast<int>(req.body.size() / 4);
    if (estimated < 1) estimated = 1;
    resp.status_code = 200;
    resp.body = std::format(R"({{"input_tokens":{}}})", estimated);
    return 0;
}

}  // namespace gateway::server
