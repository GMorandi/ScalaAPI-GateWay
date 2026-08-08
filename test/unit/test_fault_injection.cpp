#include "platform/fault_injection.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace {

class EnvironmentReset {
public:
    ~EnvironmentReset() {
        unsetenv("GATEWAY_FAULT_HOOK");
        unsetenv("GATEWAY_FAULT_MARKER_DIR");
        unsetenv("GATEWAY_FAULT_REPEAT");
    }
};

}  // namespace

TEST(FaultInjection, ClaimsConfiguredPointOnlyOnce) {
    EnvironmentReset reset;
    const auto directory = std::filesystem::temp_directory_path()
        / ("scalaapi-fault-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    setenv("GATEWAY_FAULT_HOOK", "gateway.after_provider_completion", 1);
    setenv("GATEWAY_FAULT_MARKER_DIR", directory.c_str(), 1);

    EXPECT_TRUE(gateway::platform::FaultInjection::claim(
        "gateway.after_provider_completion", "request-1"));
    EXPECT_FALSE(gateway::platform::FaultInjection::claim(
        "gateway.after_provider_completion", "request-2"));
    EXPECT_FALSE(gateway::platform::FaultInjection::claim(
        "gateway.before_provider_dispatch", "request-3"));
    EXPECT_TRUE(std::filesystem::exists(directory / "gateway-after-provider-completion.claimed"));
    std::filesystem::remove_all(directory);
}

TEST(FaultInjection, RepeatModeClaimsEveryAttempt) {
    EnvironmentReset reset;
    setenv("GATEWAY_FAULT_HOOK", "gateway.before_provider_dispatch", 1);
    setenv("GATEWAY_FAULT_REPEAT", "true", 1);

    EXPECT_TRUE(gateway::platform::FaultInjection::claim(
        "gateway.before_provider_dispatch", "request-1"));
    EXPECT_TRUE(gateway::platform::FaultInjection::claim(
        "gateway.before_provider_dispatch", "request-2"));
}
