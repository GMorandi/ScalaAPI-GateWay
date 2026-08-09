#include <gtest/gtest.h>
#include "forwarder/retry_policy.h"
#include "forwarder/failover.h"
#include "dispatch/capnp_dispatch_client.h"

using namespace gateway::forwarder;

TEST(PlatformDispatchRetryPolicy, OnlyTransportLossIsRetryable) {
    gateway::dispatch::DispatchResult result;
    result.outcome = gateway::dispatch::DispatchResult::Outcome::Rejected;
    result.reject_code = gateway::dispatch::kPlatformUnavailableRejectCode;
    EXPECT_TRUE(gateway::dispatch::is_retryable_platform_dispatch(result));

    result.reject_code = 8;
    EXPECT_FALSE(gateway::dispatch::is_retryable_platform_dispatch(result));
    result.outcome = gateway::dispatch::DispatchResult::Outcome::Ok;
    result.reject_code = gateway::dispatch::kPlatformUnavailableRejectCode;
    EXPECT_FALSE(gateway::dispatch::is_retryable_platform_dispatch(result));
}

TEST(PlatformDispatchRetryPolicy, BackoffIsBoundedAndDeterministic) {
    EXPECT_EQ(gateway::dispatch::platform_dispatch_retry_delay_ms(1), 50);
    EXPECT_EQ(gateway::dispatch::platform_dispatch_retry_delay_ms(2), 100);
    EXPECT_EQ(gateway::dispatch::platform_dispatch_retry_delay_ms(5), 800);
    EXPECT_EQ(gateway::dispatch::platform_dispatch_retry_delay_ms(6), 1000);
    EXPECT_EQ(gateway::dispatch::platform_dispatch_retry_delay_ms(100), 1000);
}

TEST(ContentPolicyDisposition, AllowsOnlyAnEvaluatedPassingDecision) {
    gateway::dispatch::ContentPolicyResult result;
    result.evaluated = true;
    result.allowed = true;
    EXPECT_EQ(gateway::dispatch::content_policy_disposition(result),
        gateway::dispatch::ContentPolicyDisposition::Allow);
}

TEST(ContentPolicyDisposition, BlocksAnEvaluatedDenial) {
    gateway::dispatch::ContentPolicyResult result;
    result.evaluated = true;
    result.allowed = false;
    EXPECT_EQ(gateway::dispatch::content_policy_disposition(result),
        gateway::dispatch::ContentPolicyDisposition::Block);
}

TEST(ContentPolicyDisposition, FailsClosedWhenPlatformDidNotEvaluate) {
    gateway::dispatch::ContentPolicyResult result;
    result.evaluated = false;
    result.allowed = true;
    EXPECT_EQ(gateway::dispatch::content_policy_disposition(result),
        gateway::dispatch::ContentPolicyDisposition::FailClosed);
}

TEST(RetryPolicy, ComputeDelayExponential) {
    RetryPolicy policy;
    policy.base_delay_ms = 100;
    policy.max_delay_ms = 10000;

    EXPECT_EQ(policy.compute_delay(0), 100u);
    EXPECT_EQ(policy.compute_delay(1), 200u);
    EXPECT_EQ(policy.compute_delay(2), 400u);
    EXPECT_EQ(policy.compute_delay(3), 800u);
    EXPECT_EQ(policy.compute_delay(4), 1600u);
}

TEST(RetryPolicy, ComputeDelayCapped) {
    RetryPolicy policy;
    policy.base_delay_ms = 300;
    policy.max_delay_ms = 3000;

    EXPECT_EQ(policy.compute_delay(0), 300u);
    EXPECT_EQ(policy.compute_delay(1), 600u);
    EXPECT_EQ(policy.compute_delay(2), 1200u);
    EXPECT_EQ(policy.compute_delay(3), 2400u);
    EXPECT_EQ(policy.compute_delay(4), 3000u);  // capped
    EXPECT_EQ(policy.compute_delay(10), 3000u); // capped
}

TEST(RetryPolicy, ComputeDelayOverflowProtection) {
    RetryPolicy policy;
    policy.base_delay_ms = 1000;
    policy.max_delay_ms = 60000;
    // attempt > 10 should not overflow
    EXPECT_EQ(policy.compute_delay(20), 60000u);
    EXPECT_EQ(policy.compute_delay(100), 60000u);
}

TEST(RetryPolicy, RetryableStatuses) {
    RetryPolicy policy;
    EXPECT_TRUE(policy.is_retryable_status(401));
    EXPECT_TRUE(policy.is_retryable_status(403));
    EXPECT_TRUE(policy.is_retryable_status(429));
    EXPECT_TRUE(policy.is_retryable_status(529));
    EXPECT_TRUE(policy.is_retryable_status(500));
    EXPECT_TRUE(policy.is_retryable_status(502));
    EXPECT_TRUE(policy.is_retryable_status(503));
}

TEST(RetryPolicy, NonRetryableStatuses) {
    RetryPolicy policy;
    EXPECT_FALSE(policy.is_retryable_status(200));
    EXPECT_FALSE(policy.is_retryable_status(400));
    EXPECT_FALSE(policy.is_retryable_status(404));
    EXPECT_FALSE(policy.is_retryable_status(409));
    EXPECT_FALSE(policy.is_retryable_status(422));
}

TEST(FailoverController, ContinueWithinRetries) {
    FailoverController ctrl(10, 3);
    EXPECT_EQ(ctrl.handle_error(1, 500), FailoverController::Action::Continue);
    EXPECT_EQ(ctrl.handle_error(1, 500), FailoverController::Action::Continue);
    EXPECT_EQ(ctrl.handle_error(1, 500), FailoverController::Action::Continue);
}

TEST(FailoverController, SwitchAfterMaxRetries) {
    FailoverController ctrl(10, 2);
    ctrl.handle_error(1, 500);
    ctrl.handle_error(1, 500);
    auto action = ctrl.handle_error(1, 500);
    EXPECT_EQ(action, FailoverController::Action::SwitchAccount);
    EXPECT_TRUE(ctrl.failed_accounts().count(1));
}

TEST(FailoverController, ExhaustedAfterMaxSwitches) {
    FailoverController ctrl(2, 1);
    // Account 1: 1 retry then switch
    ctrl.handle_error(1, 500);
    ctrl.handle_error(1, 500);  // switch 1
    // Account 2: 1 retry then switch
    ctrl.handle_error(2, 500);
    ctrl.handle_error(2, 500);  // switch 2
    // Account 3: 1 retry then switch -> exceeds max_switches=2
    ctrl.handle_error(3, 500);
    auto action = ctrl.handle_error(3, 500);  // switch 3 > max 2
    EXPECT_EQ(action, FailoverController::Action::Exhausted);
}

TEST(FailoverController, IndependentAccountTracking) {
    FailoverController ctrl(10, 2);
    ctrl.handle_error(1, 500);
    ctrl.handle_error(2, 500);
    ctrl.handle_error(1, 500);
    // Account 1 has 2 retries, account 2 has 1
    EXPECT_EQ(ctrl.handle_error(1, 500), FailoverController::Action::SwitchAccount);
    EXPECT_EQ(ctrl.handle_error(2, 500), FailoverController::Action::Continue);
}

TEST(FailoverController, FailedAccountsAccumulate) {
    FailoverController ctrl(10, 1);
    ctrl.handle_error(1, 500);
    ctrl.handle_error(1, 500);  // fails account 1
    ctrl.handle_error(2, 500);
    ctrl.handle_error(2, 500);  // fails account 2

    auto& failed = ctrl.failed_accounts();
    EXPECT_EQ(failed.size(), 2u);
    EXPECT_TRUE(failed.count(1));
    EXPECT_TRUE(failed.count(2));
    EXPECT_EQ(ctrl.switch_count(), 2);
}

TEST(FailoverController, AuthenticationFailureSwitchesImmediately) {
    FailoverController ctrl(3, 3);
    EXPECT_EQ(ctrl.handle_error(11, 401), FailoverController::Action::SwitchAccount);
    EXPECT_TRUE(ctrl.failed_accounts().contains(11));
}

TEST(FailoverController, RateLimitSwitchesImmediately) {
    FailoverController ctrl(3, 3);
    EXPECT_EQ(ctrl.handle_error(12, 429), FailoverController::Action::SwitchAccount);
    EXPECT_TRUE(ctrl.failed_accounts().contains(12));
}
