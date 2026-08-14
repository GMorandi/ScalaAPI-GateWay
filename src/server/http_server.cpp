#include "server/http_server.h"
#include "server/router.h"
#include "server/websocket.h"
#include "server/capability_registry.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "platform/logging.h"

#include <photon/photon.h>
#include <photon/net/socket.h>
#include <photon/net/http/server.h>
#include <photon/net/http/message.h>
#include <photon/net/http/websocket.h>
#include <photon/thread/thread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>

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

static std::string peer_ip(photon::net::http::Request& req) {
    auto* stream = req.get_socket_stream();
    if (!stream) return "";
    auto peer = stream->getpeername();
    char buffer[INET6_ADDRSTRLEN]{};
    if (peer.addr.is_ipv4()) {
        in_addr address{peer.addr.to_nl()};
        if (::inet_ntop(AF_INET, &address, buffer, sizeof(buffer))) return buffer;
    } else if (::inet_ntop(AF_INET6, &peer.addr.addr, buffer, sizeof(buffer))) {
        return buffer;
    }
    return "";
}

static bool ipv4_in_cidrs(std::string_view ip, std::string_view cidrs) {
    in_addr peer{};
    std::string ip_string(ip);
    if (::inet_pton(AF_INET, ip_string.c_str(), &peer) != 1) return false;
    auto peer_host = ntohl(peer.s_addr);
    size_t start = 0;
    while (start < cidrs.size()) {
        auto end = cidrs.find(',', start);
        if (end == std::string_view::npos) end = cidrs.size();
        auto entry = cidrs.substr(start, end - start);
        while (!entry.empty() && entry.front() == ' ') entry.remove_prefix(1);
        while (!entry.empty() && entry.back() == ' ') entry.remove_suffix(1);
        auto slash = entry.find('/');
        auto address_text = entry.substr(0, slash);
        int prefix = slash == std::string_view::npos ? 32
            : std::atoi(std::string(entry.substr(slash + 1)).c_str());
        in_addr network{};
        std::string network_string(address_text);
        if (prefix >= 0 && prefix <= 32
            && ::inet_pton(AF_INET, network_string.c_str(), &network) == 1) {
            uint32_t mask = prefix == 0 ? 0 : 0xffffffffu << (32 - prefix);
            if ((peer_host & mask) == (ntohl(network.s_addr) & mask)) return true;
        }
        start = end + 1;
    }
    return false;
}

static int http_handler(void* self, photon::net::http::Request& req,
                        photon::net::http::Response& resp,
                        std::string_view) {
    auto* ctx = static_cast<HandlerContext*>(self);
    if (auto* stream = req.get_socket_stream()) stream->timeout(30'000'000);

    auto target = req.target();
    auto path = target.substr(0, target.find('?'));
    auto query = target.substr(path.size());
    auto auth_hdr = req.headers["Authorization"];
    auto api_key_hdr = req.headers["X-Api-Key"];

    auto upgrade_hdr = req.headers["Upgrade"];
    auto connection_hdr = req.headers["Connection"];
    if (is_websocket_upgrade(upgrade_hdr, connection_hdr)) {
        auto capability = match_capability("GET", path);
        if (!capability.spec || !capability.spec->realtime) {
            resp.set_result(404);
            constexpr std::string_view message =
                R"({"error":{"type":"not_found_error","message":"Unknown or unsupported WebSocket endpoint"}})";
            resp.headers.insert("Content-Type", "application/json");
            resp.headers.content_length(message.size());
            resp.write(message.data(), message.size());
            return 0;
        }
        const bool bearer_auth = auth_hdr.starts_with("Bearer ") && auth_hdr.size() > 7;
        if (!bearer_auth && api_key_hdr.empty()) {
            resp.set_result(401);
            constexpr std::string_view message =
                R"({"error":{"type":"authentication_error","message":"Missing API key"}})";
            resp.headers.insert("Content-Type", "application/json");
            resp.headers.content_length(message.size());
            resp.write(message.data(), message.size());
            return 0;
        }
        auto ws_query = target.size() > path.size() ? target.substr(path.size()) : std::string_view{};
        if (!is_safe_query_string(ws_query)) {
            resp.set_result(400);
            constexpr std::string_view message =
                R"({"error":{"type":"invalid_request_error","message":"Malformed query string"}})";
            resp.headers.insert("Content-Type", "application/json");
            resp.headers.content_length(message.size());
            resp.write(message.data(), message.size());
            return 0;
        }
        auto* websocket = photon::net::http::server_accept_websocket(req, resp);
        if (!websocket) return 0;
        std::vector<std::pair<std::string, std::string>> ws_headers;
        for (auto header : req.headers) {
            if (header.first == "Accept" || header.first == "User-Agent"
                || header.first == "X-Request-ID" || header.first == "Idempotency-Key")
                ws_headers.emplace_back(header.first, header.second);
        }
        std::string ws_client_ip = peer_ip(req);
        bool ws_trusted_proxy = ipv4_in_cidrs(ws_client_ip, ctx->config.trusted_proxy_cidrs);
        auto ws_xff = req.headers["X-Forwarded-For"];
        if (ws_trusted_proxy && !ws_xff.empty()) {
            auto comma = ws_xff.find(',');
            ws_client_ip = ws_xff.substr(0, comma);
            while (!ws_client_ip.empty() && ws_client_ip.back() == ' ')
                ws_client_ip.pop_back();
        } else if (ws_trusted_proxy) {
            auto real_ip = req.headers["X-Real-IP"];
            if (!real_ip.empty()) ws_client_ip = std::string(real_ip);
        }
        HttpRequest ws_req{
            .method = "GET", .path = path,
            .query = std::string(ws_query),
            .authorization = auth_hdr, .x_api_key = api_key_hdr,
            .client_ip = ws_client_ip, .accept = req.headers["Accept"],
            .user_agent = req.headers["User-Agent"],
            .request_id = req.headers["X-Request-ID"],
            .idempotency_key = req.headers["Idempotency-Key"],
            .headers = std::move(ws_headers),
        };
        ctx->router->handle_websocket(ws_req, *websocket);
        delete websocket;
        return 0;
    }

    std::string body;
    auto content_length = req.headers["Content-Length"];
    if (!content_length.empty()) {
        size_t len = 0;
        try {
            len = std::stoull(std::string(content_length));
        } catch (...) {
            resp.set_result(400);
            resp.send();
            return 0;
        }
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
    } else if (!req.headers["Transfer-Encoding"].empty()) {
        char buffer[64 * 1024];
        for (;;) {
            auto n = req.read(buffer, sizeof(buffer));
            if (n <= 0) break;
            if (body.size() + static_cast<size_t>(n) > ctx->config.max_body_size) {
                resp.set_result(413);
                resp.send();
                return 0;
            }
            body.append(buffer, static_cast<size_t>(n));
        }
    }

    std::string client_ip = peer_ip(req);
    bool trusted_proxy = ipv4_in_cidrs(client_ip, ctx->config.trusted_proxy_cidrs);
    auto xff = req.headers["X-Forwarded-For"];
    if (trusted_proxy && !xff.empty()) {
        auto comma = xff.find(',');
        client_ip = xff.substr(0, comma);
        while (!client_ip.empty() && client_ip.back() == ' ')
            client_ip.pop_back();
    } else if (trusted_proxy) {
        auto real_ip = req.headers["X-Real-IP"];
        if (!real_ip.empty()) client_ip = std::string(real_ip);
    }

    std::vector<std::pair<std::string, std::string>> forwarded_headers;
    for (auto header : req.headers) {
        auto key = header.first;
        if (key == "Accept" || key == "User-Agent" || key == "X-Request-ID"
            || key == "Idempotency-Key") {
            forwarded_headers.emplace_back(key, header.second);
        }
    }

    HttpRequest gw_req{
        .method = verb_to_sv(req.verb()),
        .path = path,
        .query = query,
        .body = body,
        .authorization = auth_hdr,
        .x_api_key = api_key_hdr,
        .client_ip = client_ip,
        .content_type = req.headers["Content-Type"],
        .accept = req.headers["Accept"],
        .user_agent = req.headers["User-Agent"],
        .request_id = req.headers["X-Request-ID"],
        .idempotency_key = req.headers["Idempotency-Key"],
        .headers = std::move(forwarded_headers),
        .stream_timeout_ms = ctx->config.stream_timeout_ms,
        .set_client_timeout_us = [stream = req.get_socket_stream()](uint64_t timeout_us) {
            if (stream) stream->timeout(timeout_us);
        },
        .client_disconnected = [stream = req.get_socket_stream()] {
            if (!stream) return false;
            const auto fd = stream->get_underlay_fd();
            if (fd < 0) return false;
            char byte = 0;
            const auto received = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (received == 0) return true;
            return received < 0 && errno != EAGAIN && errno != EWOULDBLOCK
                && errno != EINTR;
        },
    };

    HttpResponse gw_resp;
    bool headers_sent = false;

    gw_resp.stream_write = [&](const char* data, size_t len) -> ssize_t {
        if (!headers_sent) {
            headers_sent = true;
            resp.set_result(gw_resp.status_code);
            for (const auto& [key, value] : gw_resp.headers) {
                if (key != "Content-Type") resp.headers.insert(key, value);
            }
            resp.headers.insert("Content-Type", gw_resp.content_type.empty() ? "text/event-stream" : gw_resp.content_type);
            if (gw_resp.content_type.empty() || gw_resp.content_type.starts_with("text/event-stream")) {
                // Streaming responses have no known length. Photon selects its
                // bounded body writer unless chunked framing is explicit.
                resp.headers.insert("Transfer-Encoding", "chunked");
                resp.headers.insert("Cache-Control", "no-cache");
                resp.headers.insert("Connection", "keep-alive");
            }
        }
        return resp.write(data, len);
    };

    ctx->router->handle_request(gw_req, gw_resp);

    if (headers_sent) {
        return 0;
    }

    resp.set_result(gw_resp.status_code);
    for (const auto& [key, value] : gw_resp.headers) {
        if (key != "Content-Type") resp.headers.insert(key, value);
    }
    resp.headers.insert("Content-Type", gw_resp.content_type.empty() ? "application/json" : gw_resp.content_type);
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

    auto* impl = srv->impl_.get();

    impl->sock_server = photon::net::new_tcp_socket_server();
    if (!impl->sock_server) {
        LOG_ERROR("Failed to create TCP socket server");
        return nullptr;
    }

    if (impl->sock_server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1) != 0
        || impl->sock_server->setsockopt<int>(SOL_SOCKET, SO_REUSEADDR, 1) != 0) {
        LOG_ERROR("Failed to enable SO_REUSEPORT on core {}", config.core_id);
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return nullptr;
    }

    if (impl->sock_server->bind(config.port) != 0) {
        LOG_ERROR("Failed to bind port {}", config.port);
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return nullptr;
    }

    if (impl->sock_server->listen(1024) != 0) {
        LOG_ERROR("Failed to listen on port {}", config.port);
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return nullptr;
    }

    impl->http_server = photon::net::http::new_http_server();
    if (!impl->http_server) {
        LOG_ERROR("Failed to create HTTP server");
        delete impl->sock_server;
        impl->sock_server = nullptr;
        return nullptr;
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
