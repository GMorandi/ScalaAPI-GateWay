#include "protocol/formats.h"
#include "protocol/converter.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace gateway::protocol;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    void test_##name(); \
    struct Register_##name { Register_##name() { test_##name(); } } reg_##name; \
    void test_##name()

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << "\n" \
                  << "    got: [" << _a << "] vs [" << _b << "]\n"; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { std::cerr << "  FAIL: " << #x << "\n"; return; } \
} while(0)

#define PASS() do { tests_passed++; } while(0)

TEST(openai_parse_basic) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "You are helpful"},
            {"role": "user", "content": "Hello"}
        ],
        "stream": true,
        "max_tokens": 1024,
        "temperature": 0.7
    })";

    auto req = openai::parse_request(body);
    ASSERT_EQ(req.model, "gpt-4");
    ASSERT_TRUE(req.stream);
    ASSERT_EQ(req.max_tokens, 1024);
    ASSERT_TRUE(req.temperature.has_value());
    ASSERT_EQ(req.system, "You are helpful");
    ASSERT_EQ(req.messages.size(), 1u);
    ASSERT_EQ(req.messages[0].role, "user");
    ASSERT_EQ(req.messages[0].text_content(), "Hello");
    PASS();
    std::cout << "  PASS: openai_parse_basic\n";
}

TEST(openai_parse_tool_calls) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "assistant", "content": null, "tool_calls": [
                {"id": "call_1", "type": "function", "function": {"name": "get_weather", "arguments": "{\"city\":\"NYC\"}"}}
            ]},
            {"role": "tool", "tool_call_id": "call_1", "content": "Sunny"}
        ],
        "tools": [{"type": "function", "function": {"name": "get_weather", "description": "Get weather", "parameters": {"type": "object"}}}]
    })";

    auto req = openai::parse_request(body);
    ASSERT_EQ(req.messages.size(), 2u);
    ASSERT_EQ(req.messages[0].tool_calls.size(), 1u);
    ASSERT_EQ(req.messages[0].tool_calls[0].name, "get_weather");
    ASSERT_EQ(req.messages[1].tool_call_id, "call_1");
    ASSERT_EQ(req.tools.size(), 1u);
    ASSERT_EQ(req.tools[0].name, "get_weather");
    PASS();
    std::cout << "  PASS: openai_parse_tool_calls\n";
}

TEST(anthropic_parse_basic) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "system": "Be concise",
        "messages": [
            {"role": "user", "content": "Hi there"}
        ],
        "max_tokens": 2048,
        "stream": false
    })";

    auto req = anthropic::parse_request(body);
    ASSERT_EQ(req.model, "claude-sonnet-4-20250514");
    ASSERT_EQ(req.system, "Be concise");
    ASSERT_EQ(req.max_tokens, 2048);
    ASSERT_TRUE(!req.stream);
    ASSERT_EQ(req.messages.size(), 1u);
    ASSERT_EQ(req.messages[0].text_content(), "Hi there");
    PASS();
    std::cout << "  PASS: anthropic_parse_basic\n";
}

TEST(gemini_parse_basic) {
    std::string body = R"({
        "systemInstruction": {"parts": [{"text": "System prompt"}]},
        "contents": [
            {"role": "user", "parts": [{"text": "Hello Gemini"}]},
            {"role": "model", "parts": [{"text": "Hi!"}]}
        ],
        "generationConfig": {"maxOutputTokens": 512, "temperature": 0.9}
    })";

    auto req = gemini::parse_request(body);
    ASSERT_EQ(req.system, "System prompt");
    ASSERT_EQ(req.max_tokens, 512);
    ASSERT_EQ(req.messages.size(), 2u);
    ASSERT_EQ(req.messages[0].role, "user");
    ASSERT_EQ(req.messages[1].role, "assistant");
    ASSERT_EQ(req.messages[1].text_content(), "Hi!");
    PASS();
    std::cout << "  PASS: gemini_parse_basic\n";
}

TEST(openai_to_anthropic_conversion) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "Be brief"},
            {"role": "user", "content": "What is 2+2?"}
        ],
        "max_tokens": 100,
        "stream": true
    })";

    auto result = Converter::convert_request(
        body, Format::OpenAIChatCompletions, Format::Anthropic, "claude-sonnet-4-20250514");

    auto parsed = anthropic::parse_request(result);
    ASSERT_EQ(parsed.model, "claude-sonnet-4-20250514");
    ASSERT_EQ(parsed.system, "Be brief");
    ASSERT_EQ(parsed.max_tokens, 100);
    ASSERT_TRUE(parsed.stream);
    ASSERT_EQ(parsed.messages.size(), 1u);
    ASSERT_EQ(parsed.messages[0].text_content(), "What is 2+2?");
    PASS();
    std::cout << "  PASS: openai_to_anthropic_conversion\n";
}

TEST(anthropic_to_openai_conversion) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "system": "Helpful assistant",
        "messages": [{"role": "user", "content": "Explain TCP"}],
        "max_tokens": 500
    })";

    auto result = Converter::convert_request(
        body, Format::Anthropic, Format::OpenAIChatCompletions, "gpt-4o");

    auto parsed = openai::parse_request(result);
    ASSERT_EQ(parsed.model, "gpt-4o");
    ASSERT_EQ(parsed.system, "Helpful assistant");
    ASSERT_EQ(parsed.max_tokens, 500);
    ASSERT_EQ(parsed.messages.size(), 1u);
    ASSERT_EQ(parsed.messages[0].text_content(), "Explain TCP");
    PASS();
    std::cout << "  PASS: anthropic_to_openai_conversion\n";
}

TEST(openai_stream_event_parse) {
    std::string data = R"({"model":"gpt-4","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})";
    auto delta = openai::parse_stream_event(data);
    ASSERT_TRUE(delta.type == StreamDelta::Type::TextDelta);
    ASSERT_EQ(delta.text, "Hello");
    PASS();
    std::cout << "  PASS: openai_stream_event_parse\n";
}

TEST(anthropic_stream_event_parse) {
    std::string data = R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"World"}})";
    auto delta = anthropic::parse_stream_event("content_block_delta", data);
    ASSERT_TRUE(delta.type == StreamDelta::Type::TextDelta);
    ASSERT_EQ(delta.text, "World");
    PASS();
    std::cout << "  PASS: anthropic_stream_event_parse\n";
}

TEST(stream_cross_protocol_conversion) {
    std::string openai_event = R"({"model":"gpt-4","choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]})";
    auto converted = Converter::convert_stream_event(
        openai_event, Format::OpenAIChatCompletions, Format::Anthropic);
    ASSERT_TRUE(converted.find("content_block_delta") != std::string::npos);
    ASSERT_TRUE(converted.find("Hi") != std::string::npos);
    PASS();
    std::cout << "  PASS: stream_cross_protocol_conversion\n";
}

int main() {
    std::cout << "Running protocol conversion tests...\n";
    tests_run = 9;
    std::cout << "\nResults: " << tests_passed << "/" << tests_run << " passed\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
