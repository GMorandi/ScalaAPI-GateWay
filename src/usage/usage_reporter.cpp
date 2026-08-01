#include "usage/usage_reporter.h"
#include "usage/usage_collector.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>

namespace gateway::usage {

struct UsageReporter::Impl {
    dispatch::CapnpDispatchClient* dispatch;
    std::atomic<bool> running{false};
};

std::unique_ptr<UsageReporter> UsageReporter::create(
    dispatch::CapnpDispatchClient& dispatch) {
    auto reporter = std::make_unique<UsageReporter>();
    reporter->impl_ = std::make_unique<Impl>();
    reporter->impl_->dispatch = &dispatch;
    return reporter;
}

UsageReporter::~UsageReporter() {
    stop();
}

void UsageReporter::run_loop() {
    impl_->running.store(true);
    LOG_INFO("Usage reporter started");

    while (impl_->running.load()) {
        // Drain ring buffer, batch events, send via Cap'n Proto
        photon::thread_sleep(1);
    }
}

void UsageReporter::stop() {
    impl_->running.store(false);
}

void UsageReporter::enqueue(UsageCollector& collector) {
    // Transfer events from collector to dispatch client
}

}  // namespace gateway::usage
