#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gateway::cache { class GarnetClient; }
namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::auth { class SpeculativeCache; }
namespace gateway::usage { class UsageCollector; }
namespace photon { namespace net { namespace http { class IWebSocketStream; } } }

namespace gateway::server {

using StreamWriteFn = std::function<ssize_t(const char*, size_t)>;

struct HttpRequest {
    std::string_view method;
    std::string_view path;
    std::string_view query;
    std::string_view body;
    std::string_view authorization;
    std::string_view x_api_key;
    std::string_view client_ip;
    std::string_view content_type;
    std::string_view accept;
    std::string_view user_agent;
    std::string_view request_id;
    std::string_view idempotency_key;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
    int status_code = 200;
    std::string body;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    bool stream = false;
    StreamWriteFn stream_write;
};

class Router {
public:
    static std::unique_ptr<Router> create(
        cache::GarnetClient& garnet,
        dispatch::CapnpDispatchClient& dispatch,
        auth::SpeculativeCache& auth_cache,
        usage::UsageCollector& collector);

    ~Router();

    int handle_request(const HttpRequest& req, HttpResponse& resp);
    int handle_websocket(const HttpRequest& req,
                         photon::net::http::IWebSocketStream& client);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::server
