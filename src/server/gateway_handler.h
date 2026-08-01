#pragma once

#include "server/router.h"
#include "auth/api_key_auth.h"
#include "auth/speculative_cache.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "forwarder/forwarder.h"
#include "forwarder/failover.h"
#include "forwarder/stream_pipe.h"
#include "protocol/converter.h"
#include "usage/usage_collector.h"

#include <memory>
#include <string_view>

namespace gateway::server {

class GatewayHandler {
public:
    GatewayHandler(cache::GarnetClient& garnet,
                   dispatch::CapnpDispatchClient& dispatch,
                   usage::UsageCollector& collector,
                   auth::SpeculativeCache& auth_cache);

    int handle(const HttpRequest& req, HttpResponse& resp,
               dispatch::DispatchRequest::EndpointKind endpoint);

private:
    std::string extract_api_key(const HttpRequest& req);
    std::string compute_session_hash(std::string_view body, std::string_view model);

    cache::GarnetClient& garnet_;
    dispatch::CapnpDispatchClient& dispatch_;
    usage::UsageCollector& collector_;
    auth::SpeculativeCache& auth_cache_;
    auth::ApiKeyAuth api_key_auth_;
    std::unique_ptr<forwarder::Forwarder> forwarder_;
};

}  // namespace gateway::server
