#include "usage/usage_collector.h"

namespace gateway::usage {

void UsageCollector::record(UsageEvent event) {
    ring_buffer_[write_pos_ % kRingBufferSize] = std::move(event);
    ++write_pos_;
    if (count_ < kRingBufferSize) ++count_;
}

std::vector<UsageEvent> UsageCollector::drain() {
    std::vector<UsageEvent> events;
    events.reserve(count_);
    size_t start = (write_pos_ >= count_) ? (write_pos_ - count_) : 0;
    for (size_t i = 0; i < count_; ++i) {
        events.push_back(std::move(ring_buffer_[(start + i) % kRingBufferSize]));
    }
    count_ = 0;
    return events;
}

size_t UsageCollector::pending() const {
    return count_;
}

}  // namespace gateway::usage
