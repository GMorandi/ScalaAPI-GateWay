#pragma once

#include <cstdint>

namespace gateway::forwarder {

struct RetryPolicy {
    uint32_t base_delay_ms = 300;
    uint32_t max_delay_ms = 3000;
    uint32_t max_elapsed_ms = 10000;
    int max_same_account_retries = 3;
    int max_account_switches = 10;

    uint32_t compute_delay(int attempt) const;
    bool is_retryable_status(int status_code) const;
};

}  // namespace gateway::forwarder
