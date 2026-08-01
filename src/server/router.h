#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gateway::cache { class GarnetClient; }
namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::auth { class SpeculativeCache; }
namespace gateway::usage { class UsageCollector; }

namespace gateway::server {

using StreamWriteFn = std::function<ssize_t(const char*, size_t)>;

struct HttpRequest {
    std::string_view method;
    std::string_view path;
    std::string_view body;
    std::string_view authorization;
    std::string_view x_api_key;
    std::string_view client_ip;
};

struct HttpResponse {
    int status_code = 200;
    std::string body;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::server
