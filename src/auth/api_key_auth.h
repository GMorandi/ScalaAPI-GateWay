#pragma once

#include "auth/speculative_cache.h"
#include <memory>
#include <string_view>
#include <optional>

namespace gateway::auth {

class ApiKeyAuth {
public:
    ApiKeyAuth(SpeculativeCache& cache);

    std::optional<AuthSnapshot> authenticate(std::string_view raw_key,
                                              std::string_view client_ip);

    static std::string hash_key(std::string_view raw_key);

private:
    SpeculativeCache& cache_;
};

}  // namespace gateway::auth
