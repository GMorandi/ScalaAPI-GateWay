#pragma once

#include <memory>

namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::cache { class GarnetClient; }

namespace gateway::auth {

class SpeculativeCache;

class InvalidationSubscriber {
public:
    static std::unique_ptr<InvalidationSubscriber> create(
        dispatch::CapnpDispatchClient& dispatch,
        cache::GarnetClient& garnet,
        SpeculativeCache& cache);
    ~InvalidationSubscriber();

    void run_loop();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::auth
