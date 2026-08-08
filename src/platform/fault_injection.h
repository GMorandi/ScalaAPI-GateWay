#pragma once

#include <string_view>

namespace gateway::platform {

class FaultInjection {
public:
    // Returns true exactly once for a configured hook unless repeat mode is set.
    static bool claim(std::string_view point, std::string_view correlation = {});

    // Terminates the process only when the matching opt-in hook is claimed.
    static void crash_if_configured(std::string_view point,
                                    std::string_view correlation = {});
};

}  // namespace gateway::platform
