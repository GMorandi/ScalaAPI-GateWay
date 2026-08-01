#include "auth/invalidation_subscriber.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>
#include <atomic>

namespace gateway::auth {

struct InvalidationSubscriber::Impl {
    dispatch::CapnpDispatchClient* dispatch;
    std::atomic<bool> running{false};
};

std::unique_ptr<InvalidationSubscriber> InvalidationSubscriber::create(
    dispatch::CapnpDispatchClient& dispatch) {
    auto sub = std::make_unique<InvalidationSubscriber>();
    sub->impl_ = std::make_unique<Impl>();
    sub->impl_->dispatch = &dispatch;
    return sub;
}

InvalidationSubscriber::~InvalidationSubscriber() {
    stop();
}

void InvalidationSubscriber::run_loop() {
    impl_->running.store(true);
    LOG_INFO("Invalidation subscriber started");

    while (impl_->running.load()) {
        // Cap'n Proto subscribe stream: receive InvalidationEvent
        // On event: evict from speculative cache / garnet local hints
        photon::thread_sleep(1);
    }
}

void InvalidationSubscriber::stop() {
    impl_->running.store(false);
}

}  // namespace gateway::auth
