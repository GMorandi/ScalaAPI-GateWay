#pragma once

#include <memory>

namespace gateway::dispatch { class CapnpDispatchClient; }

namespace gateway::auth {

class InvalidationSubscriber {
public:
    static std::unique_ptr<InvalidationSubscriber> create(
        dispatch::CapnpDispatchClient& dispatch);
    ~InvalidationSubscriber();

    void run_loop();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::auth
