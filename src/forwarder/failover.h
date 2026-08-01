#pragma once

#include <cstdint>
#include <unordered_set>
#include <unordered_map>

namespace gateway::forwarder {

class FailoverController {
public:
    enum class Action { Continue, SwitchAccount, Exhausted, Canceled };

    FailoverController(int max_switches = 10, int max_same_retries = 3);

    Action handle_error(int64_t account_id, int status_code);
    void mark_failed(int64_t account_id);
    const std::unordered_set<int64_t>& failed_accounts() const;
    int switch_count() const { return switch_count_; }

private:
    int max_switches_;
    int max_same_retries_;
    int switch_count_ = 0;
    std::unordered_set<int64_t> failed_accounts_;
    std::unordered_map<int64_t, int> same_account_retries_;
};

}  // namespace gateway::forwarder
