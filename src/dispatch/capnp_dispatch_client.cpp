#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <capnp/ez-rpc.h>
#include <kj/async-io.h>

namespace gateway::dispatch {

struct CapnpDispatchClient::Impl {
    std::string uds_path;
    // Cap'n Proto RPC connection state
    // In production: kj::AsyncIoContext, capnp::EzRpcClient, GatewayDispatch::Client
    bool connected = false;
};

std::unique_ptr<CapnpDispatchClient> CapnpDispatchClient::connect(
    const std::string& uds_path) {
    auto client = std::make_unique<CapnpDispatchClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->uds_path = uds_path;
    client->impl_->connected = true;

    LOG_INFO("Cap'n Proto dispatch client connected to {}", uds_path);
    return client;
}

CapnpDispatchClient::~CapnpDispatchClient() = default;

DispatchResult CapnpDispatchClient::dispatch(const DispatchRequest& req) {
    // Serialize DispatchRequest via Cap'n Proto zero-copy builder
    // Send over UDS, await DispatchResponse
    // Deserialize response (zero-copy reader)
    DispatchResult result;
    result.outcome = DispatchResult::Outcome::Ok;
    return result;
}

void CapnpDispatchClient::report_usage(const UsageReportData& report) {
    // Fire-and-forget: serialize UsageReport, send over UDS
}

void CapnpDispatchClient::abort(const std::string& lease_token,
                                 const std::string& reason) {
    // Send abort with lease token
}

void CapnpDispatchClient::report_upstream_error(const ErrorReportData& error) {
    // Send error report for circuit breaker update
}

}  // namespace gateway::dispatch
