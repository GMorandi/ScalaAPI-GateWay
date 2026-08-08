#pragma once

#include <format>
#include <string>
#include <string_view>

namespace gateway::cache::keyspace {

inline constexpr std::string_view kPrefix = "scalaapi:v1:";
inline constexpr std::string_view kInvalidationVersion =
    "scalaapi:v1:invalidation:version";

inline std::string auth(std::string_view key_hash) {
    return std::format("{}auth:{}", kPrefix, key_hash);
}

}  // namespace gateway::cache::keyspace
