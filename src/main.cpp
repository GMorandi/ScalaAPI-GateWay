#include <photon/photon.h>
#include <photon/thread/thread.h>

#include "platform/core_runtime.h"
#include "platform/logging.h"
#include "server/http_server.h"

#include <cstdlib>
#include <csignal>
#include <vector>
#include <atomic>

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
    gateway::platform::init_logging();

    int cores = std::atoi(std::getenv("GATEWAY_CORES") ?: "4");
    uint16_t port = static_cast<uint16_t>(
        std::atoi(std::getenv("GATEWAY_LISTEN_PORT") ?: "8080"));
    std::string garnet_host = std::getenv("GARNET_HOST") ?: "garnet";
    uint16_t garnet_port = static_cast<uint16_t>(
        std::atoi(std::getenv("GARNET_PORT") ?: "6379"));
    std::string garnet_password = std::getenv("GARNET_PASSWORD") ?: "";
    std::string garnet_tls_value = std::getenv("GARNET_TLS") ?: "false";
    bool garnet_use_tls = garnet_tls_value == "true" || garnet_tls_value == "1";
    std::string garnet_server_name = std::getenv("GARNET_SERVER_NAME") ?: garnet_host;
    std::string garnet_ca_cert_path = std::getenv("GARNET_CA_CERT_PATH") ?: "";
    std::string capnp_sock = std::getenv("CAPNP_UDS_PATH")
        ?: "/var/run/scalaapi/dispatch.sock";
    std::string usage_db = std::getenv("GATEWAY_USAGE_DB")
        ?: "/var/lib/scalaapi/usage-outbox.db";
    std::string trusted_proxy_cidrs = std::getenv("GATEWAY_TRUSTED_PROXY_CIDRS") ?: "";

    LOG_INFO("Starting gateway: cores={} port={} garnet={}:{} tls={} capnp={}",
             cores, port, garnet_host, garnet_port, garnet_use_tls, capnp_sock);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);

    gateway::platform::CoreRuntimeConfig config{
        .num_cores = cores,
        .listen_port = port,
        .garnet_host = garnet_host,
        .garnet_port = garnet_port,
        .garnet_password = garnet_password,
        .garnet_use_tls = garnet_use_tls,
        .garnet_server_name = garnet_server_name,
        .garnet_ca_cert_path = garnet_ca_cert_path,
        .capnp_uds_path = capnp_sock,
        .usage_db_path = usage_db,
        .trusted_proxy_cidrs = trusted_proxy_cidrs,
    };

    auto runtime = gateway::platform::CoreRuntime::create(config);
    if (!runtime) {
        LOG_ERROR("Failed to initialize core runtime");
        return 1;
    }

    runtime->start();

    while (!g_shutdown.load(std::memory_order_relaxed)) {
        photon::thread_sleep(1);
    }

    LOG_INFO("Shutting down gracefully...");
    runtime->stop();
    runtime.reset();
    photon::fini();

    return 0;
}
