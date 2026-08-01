#include "server/http_server.h"
#include "server/router.h"
#include "server/websocket.h"
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

    auto upgrade_hdr = req.headers["Upgrade"];
    auto connection_hdr = req.headers["Connection"];
    if (is_websocket_upgrade(upgrade_hdr, connection_hdr)) {
        auto ws_key = req.headers["Sec-WebSocket-Key"];
        auto accept = compute_websocket_accept(ws_key);

        resp.set_result(101);
        resp.headers.insert("Upgrade", "websocket");
        resp.headers.insert("Connection", "Upgrade");
        resp.headers.insert("Sec-WebSocket-Accept", accept);
        resp.send();

        LOG_INFO("WebSocket upgrade accepted for path {}", path);

        uint8_t buf[65536];
        for (;;) {
            ssize_t n = req.read(buf, sizeof(buf));
            if (n <= 0) break;

            WsFrame frame;
            size_t consumed = 0;
            if (!parse_ws_frame(buf, static_cast<size_t>(n), frame, consumed))
                break;

            if (frame.opcode == 0x8) {
                auto close_frame = encode_ws_frame(0x8, "");
                resp.write(close_frame.data(), close_frame.size());
                break;
            }

            if (frame.opcode == 0x9) {
                auto pong = encode_ws_frame(0xA, frame.payload);
                resp.write(pong.data(), pong.size());
                continue;
            }

            auto reply = encode_ws_frame(frame.opcode, frame.payload);
            resp.write(reply.data(), reply.size());
        }
        return 0;
    }

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

    std::string client_ip;
    auto xff = req.headers["X-Forwarded-For"];
    if (!xff.empty()) {
        auto comma = xff.find(',');
        client_ip = xff.substr(0, comma);
        while (!client_ip.empty() && client_ip.back() == ' ')
            client_ip.pop_back();
    } else {
        auto real_ip = req.headers["X-Real-IP"];
        if (!real_ip.empty()) client_ip = std::string(real_ip);
    }

    HttpRequest gw_req{
        .method = verb_to_sv(req.verb()),
        .path = path,
        .body = body,
        .authorization = auth_hdr,
        .x_api_key = api_key_hdr,
        .client_ip = client_ip,
    };

    HttpResponse gw_resp;
    bool headers_sent = false;

    gw_resp.stream_write = [&](const char* data, size_t len) -> ssize_t {
        if (!headers_sent) {
            headers_sent = true;
            resp.set_result(200);
            resp.headers.insert("Content-Type", "text/event-stream");
            resp.headers.insert("Cache-Control", "no-cache");
            resp.headers.insert("Connection", "keep-alive");
        }
        return resp.write(data, len);
    };

    ctx->router->handle_request(gw_req, gw_resp);

    if (headers_sent) {
        return 0;
    }

    resp.set_result(gw_resp.status_code);
    resp.headers.insert("Content-Type", "application/json");
    resp.headers.content_length(gw_resp.body.size());

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
