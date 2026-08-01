#include "server/http_server.h"
#include "server/router.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "platform/logging.h"

#include <photon/photon.h>
#include <photon/net/socket.h>
#include <photon/net/http/server.h>
#include <photon/net/http/message.h>
#include <photon/thread/thread.h>

namespace gateway::server {

struct HandlerContext {
    HttpServerConfig config;
    std::unique_ptr<Router> router;
};

struct HttpServer::Impl {
    HandlerContext ctx;
    photon::net::ISocketServer* sock_server = nullptr;
    photon::net::http::HTTPServer* http_server = nullptr;
};

static std::string_view verb_to_sv(photon::net::http::Verb v) {
    using V = photon::net::http::Verb;
    switch (v) {
        case V::GET: return "GET";
        case V::POST: return "POST";
        case V::PUT: return "PUT";
        case V::DELETE: return "DELETE";
        case V::PATCH: return "PATCH";
        case V::HEAD: return "HEAD";
        case V::OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

static int http_handler(void* self, photon::net::http::Request& req,
                        photon::net::http::Response& resp,
                        std::string_view) {
    auto* ctx = static_cast<HandlerContext*>(self);

    auto target = req.target();
    auto path = target.substr(0, target.find('?'));
    auto auth_hdr = req.headers["Authorization"];
    auto api_key_hdr = req.headers["X-Api-Key"];

    std::string body;
    auto content_length = req.headers["Content-Length"];
    if (!content_length.empty()) {
        size_t len = std::stoul(std::string(content_length));
        if (len > ctx->config.max_body_size) {
            resp.set_result(413);
            resp.headers.insert("Content-Type", "application/json");
            const char* err = R"({"error":"request too large"})";
            resp.headers.content_length(strlen(err));
            resp.write(err, strlen(err));
            return 0;
        }
        body.resize(len);
        size_t total = 0;
        while (total < len) {
            ssize_t n = req.read(body.data() + total, len - total);
            if (n <= 0) break;
            total += n;
        }
        body.resize(total);
    }

    HttpRequest gw_req{
        .method = verb_to_sv(req.verb()),
        .path = path,
        .body = body,
        .authorization = auth_hdr,
        .x_api_key = api_key_hdr,
        .client_ip = "",
    };

    HttpResponse gw_resp;
    ctx->router->handle_request(gw_req, gw_resp);

    resp.set_result(gw_resp.status_code);
    if (gw_resp.stream) {
        resp.headers.insert("Content-Type", "text/event-stream");
        resp.headers.insert("Cache-Control", "no-cache");
        resp.headers.insert("Connection", "keep-alive");
    } else {
        resp.headers.insert("Content-Type", "application/json");
        resp.headers.content_length(gw_resp.body.size());
    }

    if (!gw_resp.body.empty()) {
        resp.write(gw_resp.body.data(), gw_resp.body.size());
    } else {
        resp.send();
    }

    return 0;
}

std::unique_ptr<HttpServer> HttpServer::create(
    const HttpServerConfig& config,
    cache::GarnetClient& garnet,
    dispatch::CapnpDispatchClient& dispatch,
    auth::SpeculativeCache& auth_cache,
    usage::UsageCollector& collector) {

    auto srv = std::make_unique<HttpServer>();
    srv->impl_ = std::make_unique<Impl>();
    srv->impl_->ctx.config = config;
    srv->impl_->ctx.router = Router::create(garnet, dispatch, auth_cache, collector);

    if (config.core_id != 0) {
        LOG_INFO("HTTP server: core {} skipping bind (core 0 owns port {})",
                 config.core_id, config.port);
        return srv;
    }

    auto* impl = srv->impl_.get();

    impl->sock_server = photon::net::new_tcp_socket_server();
    if (!impl->sock_server) {
        LOG_ERROR("Failed to create TCP socket server");
        return srv;
    }

    if (impl->sock_server->bind(config.port) != 0) {
        LOG_ERROR("Failed to bind port {}", config.port);
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return srv;
    }

    if (impl->sock_server->listen(1024) != 0) {
        LOG_ERROR("Failed to listen on port {}", config.port);
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return srv;
    }

    impl->http_server = photon::net::http::new_http_server();
    if (!impl->http_server) {
        LOG_ERROR("Failed to create HTTP server");
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return srv;
    }

    photon::net::http::DelegateHTTPHandler handler{&impl->ctx, http_handler};
    impl->http_server->add_handler(handler);

    impl->sock_server->set_handler(impl->http_server->get_connection_handler());
    impl->sock_server->start_loop(false);

    LOG_INFO("HTTP server listening on port {} (core {})", config.port, config.core_id);
    return srv;
}

HttpServer::~HttpServer() {
    if (impl_->sock_server) {
        impl_->sock_server->terminate();
        delete impl_->sock_server;
    }
    if (impl_->http_server) {
        delete impl_->http_server;
    }
}

}  // namespace gateway::server
