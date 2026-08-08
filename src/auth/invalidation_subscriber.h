#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace gateway::dispatch { class CapnpDispatchClient; }
namespace gateway::cache { class GarnetClient; }

namespace gateway::auth {

// Tracks the durable Garnet invalidation version. A missing key is treated as
// a cache flush signal, not as a reason to keep serving an old local cache.
class InvalidationVersionTracker {
public:
    bool observe(bool found, std::string_view version);

private:
    bool initialized_ = false;
    std::string last_version_;
};

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
