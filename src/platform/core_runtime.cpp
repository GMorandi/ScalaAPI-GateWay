#include "platform/core_runtime.h"
#include "platform/logging.h"
#include "platform/metrics.h"
#include "server/http_server.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "auth/invalidation_subscriber.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "usage/usage_reporter.h"

#include <format>
#include <stdexcept>

#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <atomic>
#include <thread>

namespace gateway::platform {

struct PerCoreState {
    std::unique_ptr<cache::GarnetClient> garnet;
    std::unique_ptr<dispatch::CapnpDispatchClient> dispatch;
    std::unique_ptr<dispatch::CapnpDispatchClient> usage_dispatch;
    std::unique_ptr<auth::SpeculativeCache> speculative_cache;
    std::unique_ptr<usage::UsageCollector> collector;
    std::unique_ptr<auth::InvalidationSubscriber> invalidation_sub;
    std::unique_ptr<usage::UsageReporter> usage_reporter;
    std::unique_ptr<server::HttpServer> http_server;
    photon::join_handle* bg_invalidation = nullptr;
    photon::join_handle* bg_usage = nullptr;
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
                impl_->config.garnet_host,
                impl_->config.garnet_port,
                impl_->config.garnet_password,
                impl_->config.garnet_use_tls,
                impl_->config.garnet_server_name,
                impl_->config.garnet_ca_cert_path);
            if (!state->garnet->ping()) {
                throw std::runtime_error(
                    std::format("Core {}: Garnet unreachable at {}:{}",
                                i, impl_->config.garnet_host, impl_->config.garnet_port));
            }
            state->dispatch = dispatch::CapnpDispatchClient::connect(
                impl_->config.capnp_uds_path);
            state->usage_dispatch = dispatch::CapnpDispatchClient::connect(
                impl_->config.capnp_uds_path);
            state->speculative_cache = auth::SpeculativeCache::create();
            auto usage_path = impl_->config.usage_db_path + ".core" + std::to_string(i);
            state->collector = std::make_unique<usage::UsageCollector>(usage_path);
            global_metrics().usage_events_buffered.fetch_add(
                state->collector->pending(), std::memory_order_relaxed);
            state->invalidation_sub = auth::InvalidationSubscriber::create(
                *state->dispatch, *state->garnet, *state->speculative_cache);
            state->usage_reporter = usage::UsageReporter::create(
                *state->usage_dispatch, *state->collector);

            server::HttpServerConfig http_cfg{
                .port = impl_->config.listen_port,
                .core_id = i,
                .trusted_proxy_cidrs = impl_->config.trusted_proxy_cidrs,
                .stream_timeout_ms = impl_->config.stream_timeout_ms,
            };
            state->http_server = server::HttpServer::create(
                http_cfg, *state->garnet, *state->dispatch,
                *state->speculative_cache, *state->collector);
            if (!state->http_server) {
                throw std::runtime_error(
                    std::format("Core {}: failed to create HTTP server on port {}",
                                i, impl_->config.listen_port));
            }

            state->running.store(true);

            auto* inv_sub = state->invalidation_sub.get();
            state->bg_invalidation = photon::thread_enable_join(
                photon::thread_create11([inv_sub]() { inv_sub->run_loop(); }));

            auto* reporter = state->usage_reporter.get();
            state->bg_usage = photon::thread_enable_join(
                photon::thread_create11([reporter]() { reporter->run_loop(); }));

            impl_->core_states[i] = std::move(state);

            LOG_INFO("Core {} initialized", i);

            while (!impl_->shutdown.load(std::memory_order_relaxed)) {
                photon::thread_sleep(1);
            }

            auto& core = impl_->core_states[i];
            core->http_server.reset();
            core->invalidation_sub->stop();
            core->usage_reporter->stop();
            if (core->bg_invalidation) photon::thread_join(core->bg_invalidation);
            if (core->bg_usage) photon::thread_join(core->bg_usage);
            core->invalidation_sub.reset();
            core->usage_reporter.reset();
            core->collector.reset();
            core->speculative_cache.reset();
            core->usage_dispatch.reset();
            core->dispatch.reset();
            core->garnet.reset();
            core->running.store(false);
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
