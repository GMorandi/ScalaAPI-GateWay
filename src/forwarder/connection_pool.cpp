#include "forwarder/connection_pool.h"

namespace gateway::forwarder {

struct ConnectionPool::Impl {
    size_t max_per_host;
};

std::unique_ptr<ConnectionPool> ConnectionPool::create(size_t max_per_host) {
    auto pool = std::make_unique<ConnectionPool>();
    pool->impl_ = std::make_unique<Impl>();
    pool->impl_->max_per_host = max_per_host;
    return pool;
}

ConnectionPool::~ConnectionPool() = default;

void* ConnectionPool::acquire(const std::string& host, uint16_t port, bool tls) {
    return nullptr;
}

void ConnectionPool::release(void* conn) {}

}  // namespace gateway::forwarder
