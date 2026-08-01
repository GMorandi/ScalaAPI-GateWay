#include "auth/invalidation_subscriber.h"
#include "auth/speculative_cache.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>
#include <atomic>
#include <string>

namespace gateway::auth {

static constexpr const char* kVersionKey = "invalidation:version";
static constexpr int kPollIntervalSec = 2;

struct InvalidationSubscriber::Impl {
    dispatch::CapnpDispatchClient* dispatch;
    cache::GarnetClient* garnet;
    SpeculativeCache* cache;
    std::string last_version;
    std::atomic<bool> running{false};
};

std::unique_ptr<InvalidationSubscriber> InvalidationSubscriber::create(
    dispatch::CapnpDispatchClient& dispatch,
    cache::GarnetClient& garnet,
    SpeculativeCache& cache) {
    auto sub = std::make_unique<InvalidationSubscriber>();
    sub->impl_ = std::make_unique<Impl>();
    sub->impl_->dispatch = &dispatch;
    sub->impl_->garnet = &garnet;
    sub->impl_->cache = &cache;
    return sub;
}

InvalidationSubscriber::~InvalidationSubscriber() {
    stop();
}

void InvalidationSubscriber::run_loop() {
    impl_->running.store(true);
    LOG_INFO("Invalidation subscriber started");

    auto resp = impl_->garnet->get(kVersionKey);
    if (resp.found) {
        impl_->last_version = resp.value;
    }

    while (impl_->running.load()) {
        photon::thread_sleep(kPollIntervalSec);
        if (!impl_->running.load()) break;

        auto r = impl_->garnet->get(kVersionKey);
        if (!r.found) continue;

        if (r.value != impl_->last_version) {
            LOG_INFO("Invalidation version changed: {} -> {}, flushing cache",
                     impl_->last_version, r.value);
            impl_->cache->evict_all();
            impl_->last_version = r.value;
        }
    }

    LOG_INFO("Invalidation subscriber stopped");
}

void InvalidationSubscriber::stop() {
    impl_->running.store(false);
}

}  // namespace gateway::auth
