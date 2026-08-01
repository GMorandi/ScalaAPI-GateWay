#include "platform/metrics.h"

namespace gateway::platform {

static Metrics g_metrics;

Metrics& global_metrics() {
    return g_metrics;
}

}  // namespace gateway::platform
