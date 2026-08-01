#include <gtest/gtest.h>
#include "protocol/formats.h"
#include "protocol/converter.h"

using namespace gateway::protocol;

TEST(OpenAIParse, BasicRequest) {
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
    EXPECT_EQ(req.model, "gpt-4");
    EXPECT_TRUE(req.stream);
    EXPECT_EQ(req.max_tokens, 1024);
    ASSERT_TRUE(req.temperature.has_value());
    EXPECT_DOUBLE_EQ(*req.temperature, 0.7);
    EXPECT_EQ(req.system, "You are helpful");
    ASSERT_EQ(req.messages.size(), 1u);
    EXPECT_EQ(req.messages[0].role, "user");
    EXPECT_EQ(req.messages[0].text_content(), "Hello");
}

TEST(OpenAIParse, ToolCalls) {
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
    EXPECT_EQ(req.messages[0].tool_calls[0].name, "get_weather");
    EXPECT_EQ(req.messages[0].tool_calls[0].id, "call_1");
    EXPECT_EQ(req.messages[1].tool_call_id, "call_1");
    ASSERT_EQ(req.tools.size(), 1u);
    EXPECT_EQ(req.tools[0].name, "get_weather");
    EXPECT_EQ(req.tools[0].description, "Get weather");
}

TEST(OpenAIParse, MultipleSystemMessages) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "First"},
            {"role": "developer", "content": "Second"},
            {"role": "user", "content": "Hi"}
        ]
    })";

    auto req = openai::parse_request(body);
    EXPECT_EQ(req.system, "First\nSecond");
    ASSERT_EQ(req.messages.size(), 1u);
}

TEST(OpenAIParse, StopSequences) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [{"role": "user", "content": "Hi"}],
        "stop": ["\n", "END"]
    })";

    auto req = openai::parse_request(body);
    ASSERT_EQ(req.stop.size(), 2u);
    EXPECT_EQ(req.stop[0], "\n");
    EXPECT_EQ(req.stop[1], "END");
}

TEST(AnthropicParse, BasicRequest) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "system": "Be concise",
        "messages": [{"role": "user", "content": "Hi there"}],
        "max_tokens": 2048,
        "stream": false
    })";

    auto req = anthropic::parse_request(body);
    EXPECT_EQ(req.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(req.system, "Be concise");
    EXPECT_EQ(req.max_tokens, 2048);
    EXPECT_FALSE(req.stream);
    ASSERT_EQ(req.messages.size(), 1u);
    EXPECT_EQ(req.messages[0].text_content(), "Hi there");
}

TEST(AnthropicParse, SystemAsArray) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "system": [{"type": "text", "text": "Part1"}, {"type": "text", "text": "Part2"}],
        "messages": [{"role": "user", "content": "Hi"}],
        "max_tokens": 100
    })";

    auto req = anthropic::parse_request(body);
    EXPECT_EQ(req.system, "Part1\nPart2");
}

TEST(AnthropicParse, ToolUseBlocks) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "messages": [
            {"role": "assistant", "content": [
                {"type": "text", "text": "Let me check"},
                {"type": "tool_use", "id": "tu_1", "name": "search", "input": {"q": "test"}}
            ]},
            {"role": "user", "content": [
                {"type": "tool_result", "tool_use_id": "tu_1", "content": "Found it"}
            ]}
        ],
        "max_tokens": 100,
        "tools": [{"name": "search", "description": "Search", "input_schema": {"type": "object"}}]
    })";

    auto req = anthropic::parse_request(body);
    ASSERT_EQ(req.messages.size(), 2u);
    ASSERT_EQ(req.messages[0].tool_calls.size(), 1u);
    EXPECT_EQ(req.messages[0].tool_calls[0].name, "search");
    EXPECT_EQ(req.messages[1].tool_call_id, "tu_1");
    ASSERT_EQ(req.tools.size(), 1u);
    EXPECT_EQ(req.tools[0].name, "search");
}

TEST(GeminiParse, BasicRequest) {
    std::string body = R"({
        "systemInstruction": {"parts": [{"text": "System prompt"}]},
        "contents": [
            {"role": "user", "parts": [{"text": "Hello Gemini"}]},
            {"role": "model", "parts": [{"text": "Hi!"}]}
        ],
        "generationConfig": {"maxOutputTokens": 512, "temperature": 0.9}
    })";

    auto req = gemini::parse_request(body);
    EXPECT_EQ(req.system, "System prompt");
    EXPECT_EQ(req.max_tokens, 512);
    ASSERT_EQ(req.messages.size(), 2u);
    EXPECT_EQ(req.messages[0].role, "user");
    EXPECT_EQ(req.messages[1].role, "assistant");
    EXPECT_EQ(req.messages[1].text_content(), "Hi!");
}

TEST(GeminiParse, FunctionCall) {
    std::string body = R"({
        "contents": [
            {"role": "model", "parts": [{"functionCall": {"name": "get_time", "args": {"tz": "UTC"}}}]},
            {"role": "user", "parts": [{"functionResponse": {"name": "get_time", "response": {"time": "12:00"}}}]}
        ]
    })";

    auto req = gemini::parse_request(body);
    ASSERT_EQ(req.messages.size(), 2u);
    ASSERT_EQ(req.messages[0].tool_calls.size(), 1u);
    EXPECT_EQ(req.messages[0].tool_calls[0].name, "get_time");
    EXPECT_EQ(req.messages[1].tool_call_id, "get_time");
}

TEST(Conversion, OpenAIToAnthropic) {
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
    EXPECT_EQ(parsed.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(parsed.system, "Be brief");
    EXPECT_EQ(parsed.max_tokens, 100);
    EXPECT_TRUE(parsed.stream);
    ASSERT_EQ(parsed.messages.size(), 1u);
    EXPECT_EQ(parsed.messages[0].text_content(), "What is 2+2?");
}

TEST(Conversion, AnthropicToOpenAI) {
    std::string body = R"({
        "model": "claude-sonnet-4-20250514",
        "system": "Helpful assistant",
        "messages": [{"role": "user", "content": "Explain TCP"}],
        "max_tokens": 500
    })";

    auto result = Converter::convert_request(
        body, Format::Anthropic, Format::OpenAIChatCompletions, "gpt-4o");

    auto parsed = openai::parse_request(result);
    EXPECT_EQ(parsed.model, "gpt-4o");
    EXPECT_EQ(parsed.system, "Helpful assistant");
    EXPECT_EQ(parsed.max_tokens, 500);
    ASSERT_EQ(parsed.messages.size(), 1u);
    EXPECT_EQ(parsed.messages[0].text_content(), "Explain TCP");
}

TEST(Conversion, OpenAIToGemini) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "Sys"},
            {"role": "user", "content": "Hello"}
        ],
        "max_tokens": 256
    })";

    auto result = Converter::convert_request(
        body, Format::OpenAIChatCompletions, Format::Gemini, "gemini-pro");

    auto parsed = gemini::parse_request(result);
    EXPECT_EQ(parsed.system, "Sys");
    EXPECT_EQ(parsed.max_tokens, 256);
    ASSERT_EQ(parsed.messages.size(), 1u);
    EXPECT_EQ(parsed.messages[0].text_content(), "Hello");
}

TEST(Conversion, SameFormatPassthrough) {
    std::string body = R"({"model":"gpt-4","messages":[{"role":"user","content":"Hi"}]})";
    auto result = Converter::convert_request(
        body, Format::OpenAIChatCompletions, Format::OpenAIChatCompletions, "");
    EXPECT_EQ(result, body);
}

TEST(StreamEvent, OpenAIParse) {
    std::string data = R"({"model":"gpt-4","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})";
    auto delta = openai::parse_stream_event(data);
    EXPECT_EQ(delta.type, StreamDelta::Type::TextDelta);
    EXPECT_EQ(delta.text, "Hello");
}

TEST(StreamEvent, OpenAIDone) {
    auto delta = openai::parse_stream_event("[DONE]");
    EXPECT_EQ(delta.type, StreamDelta::Type::Done);
}

TEST(StreamEvent, AnthropicParse) {
    std::string data = R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"World"}})";
    auto delta = anthropic::parse_stream_event("content_block_delta", data);
    EXPECT_EQ(delta.type, StreamDelta::Type::TextDelta);
    EXPECT_EQ(delta.text, "World");
}

TEST(StreamEvent, AnthropicMessageStart) {
    std::string data = R"({"type":"message_start","message":{"id":"msg_1","model":"claude-sonnet-4-20250514","usage":{"input_tokens":42}}})";
    auto delta = anthropic::parse_stream_event("message_start", data);
    EXPECT_EQ(delta.type, StreamDelta::Type::MessageStart);
    EXPECT_EQ(delta.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(delta.input_tokens, 42);
}

TEST(StreamEvent, CrossProtocolOpenAIToAnthropic) {
    std::string openai_event = R"({"model":"gpt-4","choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]})";
    auto converted = Converter::convert_stream_event(
        openai_event, Format::OpenAIChatCompletions, Format::Anthropic);
    EXPECT_NE(converted.find("content_block_delta"), std::string::npos);
    EXPECT_NE(converted.find("Hi"), std::string::npos);
}

TEST(StreamEvent, SerializeOpenAI) {
    StreamDelta delta;
    delta.type = StreamDelta::Type::TextDelta;
    delta.text = "test";
    delta.model = "gpt-4";
    auto sse = openai::serialize_stream_event(delta);
    EXPECT_NE(sse.find("data: "), std::string::npos);
    EXPECT_NE(sse.find("\"content\":\"test\""), std::string::npos);
    EXPECT_NE(sse.find("\n\n"), std::string::npos);
}

TEST(StreamEvent, SerializeOpenAIDone) {
    StreamDelta delta;
    delta.type = StreamDelta::Type::Done;
    auto sse = openai::serialize_stream_event(delta);
    EXPECT_EQ(sse, "data: [DONE]\n\n");
}
