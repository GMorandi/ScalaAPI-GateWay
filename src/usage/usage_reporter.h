#pragma once

#include <memory>
#include <atomic>

namespace gateway::dispatch { class CapnpDispatchClient; }

namespace gateway::usage {

class UsageCollector;

class UsageReporter {
public:
    static std::unique_ptr<UsageReporter> create(
        dispatch::CapnpDispatchClient& dispatch,
        UsageCollector& collector);
    ~UsageReporter();

    void run_loop();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::usage
