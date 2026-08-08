#include "auth/invalidation_subscriber.h"
#include "auth/speculative_cache.h"
#include "cache/garnet_client.h"
#include "cache/garnet_keyspace.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>
#include <atomic>
#include <string>

namespace gateway::auth {

static constexpr int kPollIntervalSec = 2;

bool InvalidationVersionTracker::observe(bool found, std::string_view version) {
    if (!found) {
        const auto should_flush = initialized_;
        initialized_ = false;
        last_version_.clear();
        return should_flush;
    }

    if (!initialized_) {
        initialized_ = true;
        last_version_ = version;
        return false;
    }

    if (last_version_ == version)
        return false;

    last_version_ = version;
    return true;
}

struct InvalidationSubscriber::Impl {
    dispatch::CapnpDispatchClient* dispatch;
    cache::GarnetClient* garnet;
    SpeculativeCache* cache;
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

    InvalidationVersionTracker version_tracker;
    auto resp = impl_->garnet->get(cache::keyspace::kInvalidationVersion);
    version_tracker.observe(resp.found, resp.value);

    while (impl_->running.load()) {
        photon::thread_sleep(kPollIntervalSec);
        if (!impl_->running.load()) break;

        auto r = impl_->garnet->get(cache::keyspace::kInvalidationVersion);
        if (version_tracker.observe(r.found, r.value)) {
            LOG_INFO("Invalidation version changed or disappeared, flushing cache");
            impl_->cache->evict_all();
        }
    }

    LOG_INFO("Invalidation subscriber stopped");
}

void InvalidationSubscriber::stop() {
    if (impl_)
        impl_->running.store(false);
}

}  // namespace gateway::auth
