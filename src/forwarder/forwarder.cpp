#include "forwarder/forwarder.h"
#include "forwarder/stream_pipe.h"
#include "forwarder/connection_pool.h"
#include "platform/logging.h"

namespace gateway::forwarder {

struct Forwarder::Impl {
    ForwardConfig config;
    std::unique_ptr<ConnectionPool> pool;
};

std::unique_ptr<Forwarder> Forwarder::create(const ForwardConfig& config) {
    auto f = std::make_unique<Forwarder>();
    f->impl_ = std::make_unique<Impl>();
    f->impl_->config = config;
    f->impl_->pool = ConnectionPool::create();
    return f;
}

Forwarder::~Forwarder() = default;

ForwardResult Forwarder::forward(void* client_req, void* client_resp,
                                  const dispatch::UpstreamTarget& target,
                                  std::string_view body,
                                  bool stream) {
    ForwardResult result;
    // 1. Get/create upstream connection from pool
    // 2. Build upstream request (URL, headers, auth, model mapping)
    // 3. Send request
    // 4. If streaming: StreamPipe zero-copy relay
    // 5. If buffered: read full response, extract usage
    // 6. Return result with token counts
    return result;
}

}  // namespace gateway::forwarder
