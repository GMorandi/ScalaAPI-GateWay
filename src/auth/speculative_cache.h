#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <optional>

namespace gateway::auth {

struct AuthSnapshot {
    int64_t api_key_id = 0;
    int64_t user_id = 0;
    int64_t group_id = 0;
    std::string platform;
    std::string status;
    double quota = 0;
    double quota_used = 0;
    double rate_multiplier = 1.0;
    int concurrency = 1;
    int rpm_limit = 0;
    int64_t version = 0;
    bool claude_code_only = false;
};

class SpeculativeCache {
public:
    static std::unique_ptr<SpeculativeCache> create(size_t max_entries = 10000);
    ~SpeculativeCache();

    std::optional<AuthSnapshot> lookup(std::string_view key_hash);
    void insert(std::string_view key_hash, AuthSnapshot snapshot);
    void evict(std::string_view key_hash);
    void evict_all();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::auth
