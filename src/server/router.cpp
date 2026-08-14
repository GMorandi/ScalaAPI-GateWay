#include "server/router.h"
#include "server/capability_registry.h"
#include "server/gateway_handler.h"
#include "cache/garnet_client.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "platform/logging.h"
#include "platform/metrics.h"
#include <photon/net/http/websocket.h>

#include <format>

namespace gateway::server {

struct Router::Impl {
    cache::GarnetClient* garnet;
    dispatch::CapnpDispatchClient* dispatch;
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
    r->impl_->dispatch = &dispatch;
    r->impl_->auth_cache = &auth_cache;
    r->impl_->collector = &collector;
    r->impl_->gateway = std::make_unique<GatewayHandler>(
        garnet, dispatch, collector, auth_cache);
    return r;
}

Router::~Router() = default;

int Router::handle_request(const HttpRequest& req, HttpResponse& resp) {
    auto path = req.path;

    if (path == "/live") {
        resp.status_code = 200;
        resp.body = R"({"status":"live"})";
        return 0;
    }

    if (path == "/ready") {
        bool dispatch_ok = impl_->dispatch->is_connected();
        bool garnet_ok = impl_->garnet->ping();
        bool sqlite_ok = impl_->collector->durable();
        bool ready = dispatch_ok && garnet_ok && sqlite_ok;
        resp.status_code = ready ? 200 : 503;
        if (ready) {
            resp.body = R"({"status":"ready"})";
        } else {
            resp.body = std::format(
                R"({{"status":"not_ready","dispatch":{},"garnet":{},"sqlite":{}}})",
                dispatch_ok, garnet_ok, sqlite_ok);
        }
        return 0;
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
            "# HELP gateway_conversion_failures_total Protocol response conversion failures\n"
            "# TYPE gateway_conversion_failures_total counter\n"
            "gateway_conversion_failures_total {}\n"
            "# HELP gateway_failovers Account failovers\n"
            "# TYPE gateway_failovers counter\n"
            "gateway_failovers {}\n"
            "# HELP gateway_active_connections Active connections\n"
            "# TYPE gateway_active_connections gauge\n"
            "gateway_active_connections {}\n"
            "# HELP gateway_usage_outbox_backlog Pending durable usage events\n"
            "# TYPE gateway_usage_outbox_backlog gauge\n"
            "gateway_usage_outbox_backlog {}\n"
            "# HELP gateway_usage_dead_lettered Non-retryable usage events retained for operator review\n"
            "# TYPE gateway_usage_dead_lettered gauge\n"
            "gateway_usage_dead_lettered {}\n"
            "# HELP gateway_evidence_outbox_backlog Pending durable lease evidence\n"
            "# TYPE gateway_evidence_outbox_backlog gauge\n"
            "gateway_evidence_outbox_backlog {}\n"
            "# HELP gateway_usage_report_failures_total Failed usage report attempts\n"
            "# TYPE gateway_usage_report_failures_total counter\n"
            "gateway_usage_report_failures_total {}\n"
            "# HELP gateway_dispatch_reconnects_total Successful dispatch reconnects\n"
            "# TYPE gateway_dispatch_reconnects_total counter\n"
            "gateway_dispatch_reconnects_total {}\n",
            m.requests_total.load(), m.requests_streaming.load(),
            m.requests_failed.load(), m.dispatch_calls.load(),
            m.garnet_hits.load(), m.garnet_misses.load(),
            m.upstream_errors.load(), m.conversion_failures.load(), m.failovers.load(),
            m.active_connections.load(), m.usage_events_buffered.load(),
            impl_->collector->dead_lettered(),
            impl_->collector->pending_evidence(),
            m.usage_report_failures.load(), m.dispatch_reconnects.load());
        return 0;
    }

    auto capability = match_capability(req.method, path);
    if (capability.spec) {
        // Preserve the historical unauthenticated discovery response for
        // clients that probe the generic models endpoint without a key. A
        // keyed request is dispatched through Platform and receives the
        // provider-aware catalog.
        if (capability.spec->capability == Capability::Models
            && req.authorization.empty() && req.x_api_key.empty()) {
            auto cached = impl_->garnet->get("models:list");
            if (cached.error) {
                resp.status_code = 503;
                resp.body = R"({"error":{"type":"cache_unavailable","message":"Model catalog temporarily unavailable"}})";
                return 0;
            }
            resp.status_code = 200;
            resp.body = cached.found && !cached.value.empty()
                ? cached.value : R"({"object":"list","data":[]})";
            return 0;
        }
        return impl_->gateway->handle(req, resp, capability);
    }

    if (path_matches_any(path)) {
        resp.status_code = 405;
        resp.body = R"({"error":{"type":"method_not_allowed","message":"HTTP method is not supported for this endpoint"}})";
        return 0;
    }

    resp.status_code = 404;
    resp.body = R"({"error":{"type":"not_found_error","message":"Unknown or unsupported endpoint"}})";
    return 0;
}

int Router::handle_websocket(const HttpRequest& req,
                             photon::net::http::IWebSocketStream& client) {
    auto capability = match_capability("GET", req.path);
    if (!capability.spec || !capability.spec->realtime) {
        client.close(photon::net::http::WebSocketCloseCode::PolicyViolation,
                     "unsupported realtime endpoint");
        return -1;
    }
    return impl_->gateway->bridge_realtime(req, client);
}

}  // namespace gateway::server
