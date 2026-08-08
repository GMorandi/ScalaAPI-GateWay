#pragma once

#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace gateway::platform {

struct CoreRuntimeConfig {
    int num_cores = 4;
    uint16_t listen_port = 8080;
    std::string garnet_host;
    uint16_t garnet_port = 6379;
    std::string garnet_password;
    bool garnet_use_tls = false;
    std::string garnet_server_name;
    std::string garnet_ca_cert_path;
    std::string capnp_uds_path;
    std::string usage_db_path;
    std::string trusted_proxy_cidrs;
};

class CoreRuntime {
public:
    static std::unique_ptr<CoreRuntime> create(const CoreRuntimeConfig& config);
    ~CoreRuntime();

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void init_logging();

}  // namespace gateway::platform
