#pragma once

#include <cstdint>
#include <string>
#include <atomic>

namespace gateway::platform {

struct Metrics {
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> requests_streaming{0};
    std::atomic<uint64_t> requests_failed{0};
    std::atomic<uint64_t> dispatch_calls{0};
    std::atomic<uint64_t> garnet_hits{0};
    std::atomic<uint64_t> garnet_misses{0};
    std::atomic<uint64_t> upstream_errors{0};
    std::atomic<uint64_t> failovers{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> usage_events_buffered{0};
    std::atomic<uint64_t> usage_report_failures{0};
    std::atomic<uint64_t> dispatch_reconnects{0};
};

Metrics& global_metrics();

}  // namespace gateway::platform
