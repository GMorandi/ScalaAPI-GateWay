#include "forwarder/failover.h"

namespace gateway::forwarder {

FailoverController::FailoverController(int max_switches, int max_same_retries)
    : max_switches_(max_switches), max_same_retries_(max_same_retries) {}

FailoverController::Action FailoverController::handle_error(
    int64_t account_id, int status_code) {
    auto& retries = same_account_retries_[account_id];
    ++retries;

    if (retries <= max_same_retries_) {
        return Action::Continue;
    }

    mark_failed(account_id);
    ++switch_count_;

    if (switch_count_ > max_switches_) {
        return Action::Exhausted;
    }
    return Action::SwitchAccount;
}

void FailoverController::mark_failed(int64_t account_id) {
    failed_accounts_.insert(account_id);
}

const std::unordered_set<int64_t>& FailoverController::failed_accounts() const {
    return failed_accounts_;
}

}  // namespace gateway::forwarder
