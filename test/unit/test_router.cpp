#include <gtest/gtest.h>
#include "server/router.h"
#include "auth/speculative_cache.h"
#include "usage/usage_collector.h"
#include "cache/garnet_client.h"
#include "dispatch/capnp_dispatch_client.h"

using namespace gateway::server;

class RouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        garnet_ = gateway::cache::GarnetClient::connect("/nonexistent.sock");
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

TEST_F(RouterTest, EmbeddingsReturnNotImplemented) {
    auto resp = request("POST", "/v1/embeddings", "{}", "Bearer key");
    EXPECT_EQ(resp.status_code, 501);
}

TEST_F(RouterTest, ImagesReturnNotImplemented) {
    auto resp = request("POST", "/v1/images/generations", "{}", "Bearer key");
    EXPECT_EQ(resp.status_code, 501);
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
