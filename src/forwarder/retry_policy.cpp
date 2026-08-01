#include "forwarder/retry_policy.h"
#include <algorithm>

namespace gateway::forwarder {

uint32_t RetryPolicy::compute_delay(int attempt) const {
    uint32_t delay = base_delay_ms * (1u << std::min(attempt, 10));
    return std::min(delay, max_delay_ms);
}

bool RetryPolicy::is_retryable_status(int status_code) const {
    return status_code == 401 || status_code == 403 ||
           status_code == 429 || status_code == 529 ||
           status_code >= 500;
}

}  // namespace gateway::forwarder
