#include <gtest/gtest.h>
#include "auth/api_key_auth.h"
#include "auth/speculative_cache.h"

using namespace gateway::auth;

TEST(ApiKeyAuth, HashKeyDeterministic) {
    auto h1 = ApiKeyAuth::hash_key("sk-test-123");
    auto h2 = ApiKeyAuth::hash_key("sk-test-123");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 64u);
}

TEST(ApiKeyAuth, HashKeyDifferentInputs) {
    auto h1 = ApiKeyAuth::hash_key("key-a");
    auto h2 = ApiKeyAuth::hash_key("key-b");
    EXPECT_NE(h1, h2);
}

TEST(ApiKeyAuth, HashKeyKnownValue) {
    // SHA-256 of empty string
    auto h = ApiKeyAuth::hash_key("");
    EXPECT_EQ(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ApiKeyAuth, HashKeyHexFormat) {
    auto h = ApiKeyAuth::hash_key("anything");
    for (char c : h) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-hex char: " << c;
    }
}

TEST(SpeculativeCache, InsertAndLookup) {
    auto cache = SpeculativeCache::create(100);
    AuthSnapshot snap;
    snap.api_key_id = 42;
    snap.user_id = 7;
    snap.platform = "anthropic";

    cache->insert("hash1", snap);
    auto result = cache->lookup("hash1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->api_key_id, 42);
    EXPECT_EQ(result->user_id, 7);
    EXPECT_EQ(result->platform, "anthropic");
}

TEST(SpeculativeCache, LookupMiss) {
    auto cache = SpeculativeCache::create(100);
    auto result = cache->lookup("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST(SpeculativeCache, EvictSingle) {
    auto cache = SpeculativeCache::create(100);
    AuthSnapshot snap;
    snap.api_key_id = 1;
    cache->insert("key1", snap);
    cache->evict("key1");
    EXPECT_FALSE(cache->lookup("key1").has_value());
}

TEST(SpeculativeCache, EvictAll) {
    auto cache = SpeculativeCache::create(100);
    AuthSnapshot snap;
    cache->insert("a", snap);
    cache->insert("b", snap);
    cache->insert("c", snap);
    cache->evict_all();
    EXPECT_FALSE(cache->lookup("a").has_value());
    EXPECT_FALSE(cache->lookup("b").has_value());
    EXPECT_FALSE(cache->lookup("c").has_value());
}

TEST(SpeculativeCache, OverflowClearsAll) {
    auto cache = SpeculativeCache::create(3);
    AuthSnapshot snap;
    cache->insert("a", snap);
    cache->insert("b", snap);
    cache->insert("c", snap);
    cache->insert("d", snap);  // triggers overflow clear
    // After overflow, cache is cleared then new entry inserted
    auto result = cache->lookup("d");
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(cache->lookup("a").has_value());
}

TEST(SpeculativeCache, OverwriteExisting) {
    auto cache = SpeculativeCache::create(100);
    AuthSnapshot snap1;
    snap1.api_key_id = 1;
    cache->insert("key", snap1);

    AuthSnapshot snap2;
    snap2.api_key_id = 2;
    cache->insert("key", snap2);

    auto result = cache->lookup("key");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->api_key_id, 2);
}

TEST(ApiKeyAuth, AuthenticateWithCache) {
    auto cache = SpeculativeCache::create(100);
    ApiKeyAuth auth(*cache);

    auto key = "sk-live-abc";
    auto hash = ApiKeyAuth::hash_key(key);

    AuthSnapshot snap;
    snap.api_key_id = 99;
    snap.status = "active";
    cache->insert(hash, snap);

    auto result = auth.authenticate(key, "127.0.0.1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->api_key_id, 99);
}

TEST(ApiKeyAuth, AuthenticateCacheMiss) {
    auto cache = SpeculativeCache::create(100);
    ApiKeyAuth auth(*cache);
    auto result = auth.authenticate("unknown-key", "127.0.0.1");
    EXPECT_FALSE(result.has_value());
}
