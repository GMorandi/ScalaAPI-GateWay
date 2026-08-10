#include <gtest/gtest.h>
#include "server/router.h"
#include "server/capability_registry.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"

using namespace gateway::server;

class RouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        garnet_ = gateway::cache::GarnetClient::connect("127.0.0.1", 1);
        dispatch_ = gateway::dispatch::CapnpDispatchClient::connect("/nonexistent.sock");
        cache_ = gateway::auth::SpeculativeCache::create(100);
        collector_ = std::make_unique<gateway::usage::UsageCollector>();
        router_ = Router::create(*garnet_, *dispatch_, *cache_, *collector_);
    }

    HttpResponse request(std::string_view method, std::string_view path,
                         std::string_view body = "", std::string_view auth = "") {
        HttpRequest req{
            .method = method,
            .path = path,
            .body = body,
            .authorization = auth,
            .x_api_key = "",
            .client_ip = "127.0.0.1",
        };
        HttpResponse resp;
        router_->handle_request(req, resp);
        return resp;
    }

    std::unique_ptr<gateway::cache::GarnetClient> garnet_;
    std::unique_ptr<gateway::dispatch::CapnpDispatchClient> dispatch_;
    std::unique_ptr<gateway::auth::SpeculativeCache> cache_;
    std::unique_ptr<gateway::usage::UsageCollector> collector_;
    std::unique_ptr<Router> router_;
};

TEST_F(RouterTest, MessagesRoute) {
    auto resp = request("POST", "/v1/messages", "{}", "Bearer key");
    // Will get 401 because dispatch is not connected, but route is matched (not 404)
    EXPECT_NE(resp.status_code, 404);
}

TEST_F(RouterTest, ChatCompletionsRoute) {
    auto resp = request("POST", "/v1/chat/completions", "{}", "Bearer key");
    EXPECT_NE(resp.status_code, 404);
}

TEST_F(RouterTest, ResponsesRoute) {
    auto resp = request("POST", "/v1/responses", "{}", "Bearer key");
    EXPECT_NE(resp.status_code, 404);
}

TEST_F(RouterTest, ModelsRoute) {
    auto resp = request("GET", "/v1/models");
    EXPECT_EQ(resp.status_code, 200);
}

TEST_F(RouterTest, GeminiRoute) {
    auto resp = request("POST", "/v1beta/models/gemini-pro:generateContent", "{}", "Bearer key");
    EXPECT_NE(resp.status_code, 404);
}

TEST_F(RouterTest, EmbeddingsRouteIsDispatched) {
    auto resp = request("POST", "/v1/embeddings", "{}", "Bearer key");
    EXPECT_NE(resp.status_code, 404);
    EXPECT_NE(resp.status_code, 501);
}

TEST_F(RouterTest, ImagesRouteIsDispatched) {
    auto resp = request("POST", "/v1/images/generations", "{}", "Bearer key");
    EXPECT_NE(resp.status_code, 404);
    EXPECT_NE(resp.status_code, 501);
}

TEST_F(RouterTest, LiveDoesNotDependOnPlatform) {
    auto resp = request("GET", "/live");
    EXPECT_EQ(resp.status_code, 200);
}

TEST_F(RouterTest, UnknownRoute404) {
    auto resp = request("GET", "/unknown/path");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(RouterTest, RootPath404) {
    auto resp = request("GET", "/");
    EXPECT_EQ(resp.status_code, 404);
}

TEST_F(RouterTest, NoAuthReturns401) {
    auto resp = request("POST", "/v1/messages", R"({"model":"x","messages":[]})");
    EXPECT_EQ(resp.status_code, 401);
}

TEST(CapabilityRegistryTest, CoversApprovedMediaAndRealtimeRoutes) {
    struct Route { std::string_view method; std::string_view path; std::string_view capability; };
    constexpr Route routes[] = {
        {"POST", "/v1/embeddings", "embeddings"},
        {"POST", "/v1/images/generations", "images_sync"},
        {"POST", "/v1/images/edits/async", "images_async"},
        {"GET", "/v1/images/tasks/task_1", "images_async"},
        {"DELETE", "/v1/images/batches/batch_1", "images_batch"},
        {"POST", "/v1/videos/extensions", "videos"},
        {"GET", "/v1/videos/request_1/content", "videos"},
        {"GET", "/v1/responses", "realtime"},
        {"GET", "/v1/responses/resp_123", "responses_subpath"},
        {"DELETE", "/v1/responses/resp_123", "responses_subpath"},
        {"POST", "/v1/responses/resp_123/cancel", "responses_subpath"},
        {"POST", "/v1/live", "realtime"},
        {"GET", "/backend-api/codex/call_123", "realtime"},
        {"GET", "/v1beta/models/gemini-2.5-pro", "gemini_models"},
        {"POST", "/antigravity/v1/messages", "antigravity"},
        {"GET", "/antigravity/v1/models", "antigravity"},
        {"GET", "/antigravity/v1/usage", "antigravity"},
    };
    for (const auto& route : routes) {
        auto matched = match_capability(route.method, route.path);
        ASSERT_NE(matched.spec, nullptr) << route.path;
        EXPECT_EQ(matched.spec->name, route.capability) << route.path;
    }
}

TEST(CapabilityRegistryTest, RejectsGeminiGenerationWithoutActionSeparator) {
    auto matched = match_capability("POST", "/v1beta/models/gemini-pro/generateContent");
    EXPECT_FALSE(matched.spec);
}

TEST(CapabilityRegistryTest, RejectsUnsafeDynamicSegmentsBeforeDispatch) {
    EXPECT_EQ(match_capability("GET", "/v1/images/tasks/../../etc/passwd").spec, nullptr);
    EXPECT_EQ(match_capability("POST", "/v1/responses/../../admin").spec, nullptr);
    EXPECT_EQ(match_capability("POST", "/v1/responses/resp_1/metadata").spec, nullptr);
    EXPECT_EQ(match_capability("GET", "/v1beta/models/..:generateContent").spec, nullptr);
    EXPECT_EQ(match_capability("POST", "/v1/images/batches/batch_1/not-a-route").spec, nullptr);
    EXPECT_EQ(match_capability("GET", "/v1/videos/request_1/metadata").spec, nullptr);
    EXPECT_EQ(match_capability("GET", "/v1/responses/resp_1/cancel").spec, nullptr);
    EXPECT_EQ(match_capability("DELETE", "/v1/responses/resp_1/cancel").spec, nullptr);
}

TEST(CapabilityRegistryTest, ValidatesQueryBeforeDispatch) {
    EXPECT_TRUE(is_safe_query_string(""));
    EXPECT_TRUE(is_safe_query_string("?limit=20&after=item_1"));
    EXPECT_TRUE(is_safe_query_string("?page_token=a%2Fb"));
    EXPECT_FALSE(is_safe_query_string("limit=20"));
    EXPECT_FALSE(is_safe_query_string("?next=%2"));
    EXPECT_FALSE(is_safe_query_string("?next=x#fragment"));
    EXPECT_FALSE(is_safe_query_string("?next=x\r\nInjected: yes"));
}
