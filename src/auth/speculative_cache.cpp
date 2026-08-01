#include "auth/speculative_cache.h"
#include <unordered_map>

namespace gateway::auth {

struct SpeculativeCache::Impl {
    std::unordered_map<std::string, AuthSnapshot> entries;
    size_t max_entries;
};

std::unique_ptr<SpeculativeCache> SpeculativeCache::create(size_t max_entries) {
    auto cache = std::make_unique<SpeculativeCache>();
    cache->impl_ = std::make_unique<Impl>();
    cache->impl_->max_entries = max_entries;
    return cache;
}

SpeculativeCache::~SpeculativeCache() = default;

std::optional<AuthSnapshot> SpeculativeCache::lookup(std::string_view key_hash) {
    auto it = impl_->entries.find(std::string(key_hash));
    if (it == impl_->entries.end()) return std::nullopt;
    return it->second;
}

void SpeculativeCache::insert(std::string_view key_hash, AuthSnapshot snapshot) {
    if (impl_->entries.size() >= impl_->max_entries) {
        impl_->entries.clear();
    }
    impl_->entries[std::string(key_hash)] = std::move(snapshot);
}

void SpeculativeCache::evict(std::string_view key_hash) {
    impl_->entries.erase(std::string(key_hash));
}

void SpeculativeCache::evict_all() {
    impl_->entries.clear();
}

}  // namespace gateway::auth
