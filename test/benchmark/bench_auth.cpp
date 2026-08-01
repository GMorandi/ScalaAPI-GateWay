#include <benchmark/benchmark.h>
#include "auth/api_key_auth.h"
#include "auth/speculative_cache.h"

using namespace gateway::auth;

static void BM_HashKey_Short(benchmark::State& state) {
    std::string key = "sk-abc123def456";
    for (auto _ : state) {
        auto result = ApiKeyAuth::hash_key(key);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_HashKey_Short);

static void BM_HashKey_Long(benchmark::State& state) {
    std::string key(256, 'x');
    key = "sk-" + key;
    for (auto _ : state) {
        auto result = ApiKeyAuth::hash_key(key);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_HashKey_Long);

static void BM_CacheLookup_Hit(benchmark::State& state) {
    auto cache = SpeculativeCache::create(10000);
    AuthSnapshot snap;
    snap.api_key_id = 42;
    snap.user_id = 1;
    snap.group_id = 2;
    snap.status = "active";
    snap.version = 1;
    std::string hash = ApiKeyAuth::hash_key("sk-test-key-for-benchmark");
    cache->insert(hash, snap);

    for (auto _ : state) {
        auto result = cache->lookup(hash);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_CacheLookup_Hit);

static void BM_CacheLookup_Miss(benchmark::State& state) {
    auto cache = SpeculativeCache::create(10000);
    std::string hash = ApiKeyAuth::hash_key("sk-nonexistent-key");

    for (auto _ : state) {
        auto result = cache->lookup(hash);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_CacheLookup_Miss);

static void BM_CacheInsert(benchmark::State& state) {
    auto cache = SpeculativeCache::create(10000);
    AuthSnapshot snap;
    snap.api_key_id = 1;
    snap.user_id = 1;
    snap.group_id = 1;
    snap.status = "active";
    snap.version = 1;

    int64_t counter = 0;
    for (auto _ : state) {
        std::string hash = ApiKeyAuth::hash_key("sk-key-" + std::to_string(counter++));
        cache->insert(hash, snap);
        if (counter % 100 == 0) cache->evict_all();
    }
}
BENCHMARK(BM_CacheInsert);
