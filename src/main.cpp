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
    std::string garnet_sock = std::getenv("GARNET_UDS_PATH")
        ?: "/var/run/sub2api/garnet.sock";
    std::string capnp_sock = std::getenv("CAPNP_UDS_PATH")
        ?: "/var/run/sub2api/dispatch.sock";

    LOG_INFO("Starting gateway: cores={} port={} garnet={} capnp={}",
             cores, port, garnet_sock, capnp_sock);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    gateway::platform::CoreRuntimeConfig config{
        .num_cores = cores,
        .listen_port = port,
        .garnet_uds_path = garnet_sock,
        .capnp_uds_path = capnp_sock,
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

    return 0;
}
