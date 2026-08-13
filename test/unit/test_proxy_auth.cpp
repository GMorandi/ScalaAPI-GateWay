#include <gtest/gtest.h>
#include "forwarder/forwarder.h"
#include "dispatch/capnp_dispatch_client.h"

using namespace gateway::forwarder;
using namespace gateway::dispatch;

TEST(TargetAuthHeaderValidation, RejectsEmptyHeaders) {
    std::vector<std::pair<std::string, std::string>> headers;
    EXPECT_TRUE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, AcceptsValidAuthHeader) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer test-token-123"},
    };
    EXPECT_TRUE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsHopByHopHeaders) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Connection", "keep-alive"},
    };
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsHostHeader) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Host", "evil.example.com"},
    };
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsHeaderWithNewlines) {
    std::vector<std::string> headers_data = {
        "Authorization", "Bearer token\r\nX-Injected: evil"
    };
    std::vector<std::pair<std::string, std::string>> headers = {
        {headers_data[0], headers_data[1]},
    };
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsDuplicateHeaders) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer token1"},
        {"authorization", "Bearer token2"},
    };
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsTooManyHeaders) {
    std::vector<std::pair<std::string, std::string>> headers;
    for (int i = 0; i < 17; ++i) {
        headers.emplace_back("X-Custom-" + std::to_string(i), "value");
    }
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(TargetAuthHeaderValidation, RejectsContentLengthHeader) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Length", "999999"},
    };
    EXPECT_FALSE(validate_target_auth_headers(headers));
}

TEST(UpstreamTargetProxyCredentials, DefaultEmptyCredentials) {
    UpstreamTarget target;
    EXPECT_TRUE(target.proxy_url.empty());
    EXPECT_TRUE(target.proxy_username.empty());
    EXPECT_TRUE(target.proxy_password.empty());
}

TEST(UpstreamTargetProxyCredentials, CanSetProxyCredentials) {
    UpstreamTarget target;
    target.proxy_url = "http://proxy.example:8080";
    target.proxy_username = "user";
    target.proxy_password = "pass";
    EXPECT_EQ(target.proxy_url, "http://proxy.example:8080");
    EXPECT_EQ(target.proxy_username, "user");
    EXPECT_EQ(target.proxy_password, "pass");
}
