#include "platform/core_runtime.h"
#include "platform/logging.h"
#include "server/http_server.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "auth/invalidation_subscriber.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "usage/usage_reporter.h"

#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <atomic>
#include <thread>

namespace gateway::platform {

struct PerCoreState {
    std::unique_ptr<cache::GarnetClient> garnet;
    std::unique_ptr<dispatch::CapnpDispatchClient> dispatch;
    std::unique_ptr<auth::SpeculativeCache> speculative_cache;
    std::unique_ptr<usage::UsageCollector> collector;
    std::unique_ptr<auth::InvalidationSubscriber> invalidation_sub;
    std::unique_ptr<usage::UsageReporter> usage_reporter;
    std::unique_ptr<server::HttpServer> http_server;
    photon::thread* bg_invalidation = nullptr;
    photon::thread* bg_usage = nullptr;
    std::atomic<bool> running{false};
};

struct CoreRuntime::Impl {
    CoreRuntimeConfig config;
    std::vector<std::thread> os_threads;
    std::vector<std::unique_ptr<PerCoreState>> core_states;
    std::atomic<bool> shutdown{false};
};

std::unique_ptr<CoreRuntime> CoreRuntime::create(const CoreRuntimeConfig& config) {
    auto rt = std::make_unique<CoreRuntime>();
    rt->impl_ = std::make_unique<Impl>();
    rt->impl_->config = config;
    rt->impl_->core_states.resize(config.num_cores);
    return rt;
}

CoreRuntime::~CoreRuntime() = default;

void CoreRuntime::start() {
    auto& cfg = impl_->config;

    for (int i = 0; i < cfg.num_cores; ++i) {
        impl_->os_threads.emplace_back([this, i]() {
            photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);

            auto state = std::make_unique<PerCoreState>();

            state->garnet = cache::GarnetClient::connect(
                impl_->config.garnet_uds_path);
            state->dispatch = dispatch::CapnpDispatchClient::connect(
                impl_->config.capnp_uds_path);
            state->speculative_cache = auth::SpeculativeCache::create();
            state->collector = std::make_unique<usage::UsageCollector>();
            state->invalidation_sub = auth::InvalidationSubscriber::create(
                *state->dispatch, *state->garnet, *state->speculative_cache);
            state->usage_reporter = usage::UsageReporter::create(
                *state->dispatch, *state->collector);

            server::HttpServerConfig http_cfg{
                .port = impl_->config.listen_port,
                .core_id = i,
            };
            state->http_server = server::HttpServer::create(
                http_cfg, *state->garnet, *state->dispatch,
                *state->speculative_cache, *state->collector);

            state->running.store(true);
            impl_->core_states[i] = std::move(state);

            LOG_INFO("Core {} initialized", i);

            while (!impl_->shutdown.load(std::memory_order_relaxed)) {
                photon::thread_sleep(1);
            }

            impl_->core_states[i]->running.store(false);
            photon::fini();
        });
    }
}

void CoreRuntime::stop() {
    impl_->shutdown.store(true);
    for (auto& t : impl_->os_threads) {
        if (t.joinable()) t.join();
    }
}

}  // namespace gateway::platform
