#include "usage/usage_collector.h"

namespace gateway::usage {

void UsageCollector::record(UsageEvent event) {
    ring_buffer_[write_pos_ % kRingBufferSize] = std::move(event);
    ++write_pos_;
    if (count_ < kRingBufferSize) ++count_;
}

size_t UsageCollector::pending() const {
    return count_;
}

}  // namespace gateway::usage
