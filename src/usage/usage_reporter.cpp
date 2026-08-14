#include "usage/usage_reporter.h"
#include "usage/usage_collector.h"
#include "dispatch/capnp_dispatch_client.h"
#include "platform/logging.h"
#include "platform/metrics.h"

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
    if (impl_) stop();
}

void UsageReporter::run_loop() {
    impl_->running.store(true);
    LOG_INFO("Usage reporter started");

    while (impl_->running.load()) {
        photon::thread_sleep(kFlushIntervalSec);
        if (!impl_->running.load()) break;

        auto events = impl_->collector->peek();
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
            report.status_code = ev.status_code;
            report.input_image_count = ev.input_image_count;
            report.output_image_count = ev.output_image_count;
            report.image_size = std::move(ev.image_size);
            report.video_count = ev.video_count;
            report.video_resolution = std::move(ev.video_resolution);
            report.video_duration_seconds = ev.video_duration_seconds;
            report.realtime_duration_ms = ev.realtime_duration_ms;
            report.realtime_frames = ev.realtime_frames;
            report.disconnect_reason = std::move(ev.disconnect_reason);
            report.provider_usage_json = std::move(ev.provider_usage_json);
            report.reasoning_tokens = ev.reasoning_tokens;
            report.service_tier = std::move(ev.service_tier);
            report.upstream_endpoint = std::move(ev.upstream_endpoint);
            report.cancellation_reason = std::move(ev.cancellation_reason);
            report.media_operation_id = std::move(ev.media_operation_id);
            report.pricing_version = std::move(ev.pricing_version);
            report.response_status_code = ev.response_status_code;
            report.response_content_type = std::move(ev.response_content_type);
            report.response_body = std::move(ev.response_body);
            auto ack = impl_->dispatch->report_usage(report);
            if (ack.acknowledged()) {
                impl_->collector->acknowledge(report.lease_token);
                platform::global_metrics().usage_events_buffered.fetch_sub(
                    1, std::memory_order_relaxed);
                continue;
            }
            if (!ack.retryable) {
                impl_->collector->dead_letter(report.lease_token, ack.error_code);
                platform::global_metrics().usage_events_buffered.fetch_sub(
                    1, std::memory_order_relaxed);
                platform::global_metrics().usage_report_failures.fetch_add(
                    1, std::memory_order_relaxed);
                LOG_WARN("Dead-lettered non-retryable usage report: lease={} error={}",
                         report.lease_token, ack.error_code);
                continue;
            }
            platform::global_metrics().usage_report_failures.fetch_add(
                1, std::memory_order_relaxed);
            LOG_WARN("Usage report retained for retry: lease={} error={} retryable={}",
                     report.lease_token, ack.error_code, ack.retryable);
            // A retryable result belongs to this lease only. Do not let an
            // unrelated transient or reconciliation-needed lease block every
            // later usage event in the durable outbox.
            continue;
        }

        auto evidence_events = impl_->collector->peek_evidence();
        for (auto& ev : evidence_events) {
            auto stage = ev.stage == "output_started"
                ? dispatch::LeaseEvidenceStage::OutputStarted
                : dispatch::LeaseEvidenceStage::Forwarded;
            auto ack = impl_->dispatch->record_lease_evidence(
                ev.lease_token, stage, ev.detail);
            if (ack.acknowledged()) {
                impl_->collector->acknowledge_evidence(ev.lease_token, ev.stage);
                continue;
            }
            if (!ack.retryable) {
                impl_->collector->dead_letter_evidence(
                    ev.lease_token, ev.stage, ack.error_code);
                platform::global_metrics().usage_report_failures.fetch_add(
                    1, std::memory_order_relaxed);
                LOG_WARN("Dead-lettered non-retryable evidence: lease={} stage={} error={}",
                         ev.lease_token, ev.stage, ack.error_code);
                continue;
            }
            platform::global_metrics().usage_report_failures.fetch_add(
                1, std::memory_order_relaxed);
            LOG_WARN("Evidence retained for retry: lease={} stage={} error={}",
                     ev.lease_token, ev.stage, ack.error_code);
            continue;
        }
    }

    LOG_INFO("Usage reporter stopped");
}

void UsageReporter::stop() {
    if (impl_) impl_->running.store(false);
}

}  // namespace gateway::usage
