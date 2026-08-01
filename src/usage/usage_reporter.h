#pragma once

#include <memory>
#include <atomic>

namespace gateway::dispatch { class CapnpDispatchClient; }

namespace gateway::usage {

class UsageReporter {
public:
    static std::unique_ptr<UsageReporter> create(
        dispatch::CapnpDispatchClient& dispatch);
    ~UsageReporter();

    void run_loop();
    void stop();
    void enqueue(class UsageCollector& collector);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::usage
