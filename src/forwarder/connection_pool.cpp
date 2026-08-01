#include "forwarder/connection_pool.h"
#include "platform/logging.h"

#include <photon/net/http/client.h>

#include <mutex>
#include <unordered_map>

namespace gateway::forwarder {

struct ConnectionPool::Impl {
    size_t max_per_host;
    std::unordered_map<std::string, photon::net::http::Client*> clients;
    photon::net::http::Client* default_client = nullptr;
};

std::unique_ptr<ConnectionPool> ConnectionPool::create(size_t max_per_host) {
    auto pool = std::make_unique<ConnectionPool>();
    pool->impl_ = std::make_unique<Impl>();
    pool->impl_->max_per_host = max_per_host;
    pool->impl_->default_client = photon::net::http::new_http_client();
    return pool;
}

ConnectionPool::~ConnectionPool() {
    for (auto& [_, client] : impl_->clients) {
        delete client;
    }
    if (impl_->default_client) {
        delete impl_->default_client;
    }
}

photon::net::http::Client* ConnectionPool::get_client(const std::string& host) {
    auto it = impl_->clients.find(host);
    if (it != impl_->clients.end()) {
        return it->second;
    }

    if (impl_->clients.size() >= impl_->max_per_host) {
        return impl_->default_client;
    }

    auto* client = photon::net::http::new_http_client();
    if (!client) {
        return impl_->default_client;
    }
    impl_->clients[host] = client;
    return client;
}

}  // namespace gateway::forwarder
