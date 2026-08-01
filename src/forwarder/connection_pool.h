#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gateway::forwarder {

class ConnectionPool {
public:
    static std::unique_ptr<ConnectionPool> create(size_t max_per_host = 64);
    ~ConnectionPool();

    void* acquire(const std::string& host, uint16_t port, bool tls);
    void release(void* conn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::forwarder
