#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace photon::net::http { class Client; }

namespace gateway::forwarder {

class ConnectionPool {
public:
    static std::unique_ptr<ConnectionPool> create(size_t max_per_host = 64);
    ~ConnectionPool();

    photon::net::http::Client* get_client(const std::string& host);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::forwarder
