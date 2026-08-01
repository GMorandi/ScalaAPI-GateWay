#include "usage/usage_reporter.h"
#include "usage/usage_collector.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"

#include <photon/thread/thread.h>

namespace gateway::usage {

static constexpr int kFlushIntervalSec = 1;

struct UsageReporter::Impl {
    dispatch::CapnpDispatchClient* dispatch;
    UsageCollector* collector;
    std::atomic<bool> running{false};
};

std::unique_ptr<UsageReporter> UsageReporter::create(
    dispatch::CapnpDispatchClient& dispatch,
    UsageCollector& collector) {
    auto reporter = std::make_unique<UsageReporter>();
    reporter->impl_ = std::make_unique<Impl>();
    reporter->impl_->dispatch = &dispatch;
    reporter->impl_->collector = &collector;
    return reporter;
}

UsageReporter::~UsageReporter() {
    stop();
}

void UsageReporter::run_loop() {
    impl_->running.store(true);
    LOG_INFO("Usage reporter started");

    while (impl_->running.load()) {
        photon::thread_sleep(kFlushIntervalSec);
        if (!impl_->running.load()) break;

        auto events = impl_->collector->drain();
        if (events.empty()) continue;

        LOG_DEBUG("Flushing {} usage events", events.size());
        for (auto& ev : events) {
            dispatch::UsageReportData report;
            report.lease_token = std::move(ev.lease_token);
            report.request_id = std::move(ev.request_id);
            report.api_key_id = ev.api_key_id;
            report.user_id = ev.user_id;
            report.account_id = ev.account_id;
            report.group_id = ev.group_id;
            report.model = std::move(ev.model);
            report.upstream_model = std::move(ev.upstream_model);
            report.input_tokens = ev.input_tokens;
            report.output_tokens = ev.output_tokens;
            report.cache_create_tokens = ev.cache_create_tokens;
            report.cache_read_tokens = ev.cache_read_tokens;
            report.duration_ms = ev.duration_ms;
            report.first_token_ms = ev.first_token_ms;
            report.stream = ev.stream;
            report.client_disconnect = ev.client_disconnect;
            impl_->dispatch->report_usage(report);
        }
    }

    LOG_INFO("Usage reporter stopped");
}

void UsageReporter::stop() {
    impl_->running.store(false);
}

}  // namespace gateway::usage
