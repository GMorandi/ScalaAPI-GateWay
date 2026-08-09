#include <gtest/gtest.h>

#include "forwarder/forwarder.h"
#include "protocol/converter.h"
#include "protocol/formats.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct SseFrame {
    std::string event;
    std::string data;
};

std::string read_fixture(std::string_view name) {
    const std::string path = std::string(GATEWAY_SOURCE_DIR)
        + "/test/fixtures/protocol/" + std::string(name);
    std::ifstream input(path);
    if (!input) throw std::runtime_error("unable to read protocol fixture: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<SseFrame> parse_sse_fixture(std::string_view source) {
    std::vector<SseFrame> frames;
    size_t begin = 0;
    while (begin < source.size()) {
        auto end = source.find("\n\n", begin);
        if (end == std::string_view::npos) end = source.size();
        auto block = source.substr(begin, end - begin);
        SseFrame frame;
        size_t line_begin = 0;
        while (line_begin < block.size()) {
            auto line_end = block.find('\n', line_begin);
            if (line_end == std::string_view::npos) line_end = block.size();
            auto line = block.substr(line_begin, line_end - line_begin);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.starts_with("event: ")) frame.event = std::string(line.substr(7));
            if (line.starts_with("data: ")) frame.data = std::string(line.substr(6));
            line_begin = line_end == block.size() ? block.size() : line_end + 1;
        }
        if (!frame.data.empty()) frames.push_back(std::move(frame));
        if (end == source.size()) break;
        begin = end + 2;
    }
    return frames;
}

}  // namespace

TEST(ProtocolGolden, VersionedRequestsNormalizeIntoOneChatIR) {
    using namespace gateway::protocol;

    const auto openai = openai::parse_request(read_fixture("openai_chat_request_v1.json"));
    EXPECT_EQ(openai.model, "gpt-4o");
    EXPECT_EQ(openai.system, "You are precise.");
    EXPECT_TRUE(openai.stream);
    EXPECT_EQ(openai.max_tokens, 128);
    ASSERT_EQ(openai.messages.size(), 1u);
    EXPECT_EQ(openai.messages[0].text_content(), "Weather in Vienna?");
    ASSERT_EQ(openai.tools.size(), 1u);
    EXPECT_EQ(openai.tools[0].name, "get_weather");

    const auto responses = openai_responses::parse_request(
        read_fixture("openai_responses_request_v1.json"));
    EXPECT_EQ(responses.model, "gpt-4o");
    EXPECT_EQ(responses.system, "Use the weather tool when needed.");
    EXPECT_TRUE(responses.stream);
    EXPECT_EQ(responses.max_tokens, 256);
    EXPECT_EQ(responses.metadata_user_id, "golden-user");
    ASSERT_EQ(responses.messages.size(), 3u);
    ASSERT_EQ(responses.messages[1].tool_calls.size(), 1u);
    EXPECT_EQ(responses.messages[1].tool_calls[0].id, "call_weather");
    EXPECT_EQ(responses.messages[2].tool_call_id, "call_weather");

    const auto anthropic = anthropic::parse_request(
        read_fixture("anthropic_messages_request_v1.json"));
    EXPECT_EQ(anthropic.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(anthropic.system, "You are precise.");
    EXPECT_TRUE(anthropic.stream);
    EXPECT_EQ(anthropic.max_tokens, 256);
    EXPECT_EQ(anthropic.metadata_user_id, "golden-user");
    ASSERT_EQ(anthropic.messages.size(), 1u);
    EXPECT_EQ(anthropic.messages[0].text_content(), "Weather in Vienna?");

    const auto gemini = gemini::parse_request(
        read_fixture("gemini_generate_request_v1.json"));
    EXPECT_EQ(gemini.model, "gemini-2.0-flash");
    EXPECT_EQ(gemini.system, "You are precise.");
    EXPECT_EQ(gemini.max_tokens, 256);
    ASSERT_TRUE(gemini.temperature.has_value());
    EXPECT_DOUBLE_EQ(*gemini.temperature, 0.2);
    ASSERT_EQ(gemini.tools.size(), 1u);
    EXPECT_EQ(gemini.tools[0].name, "get_weather");
}

TEST(ProtocolGolden, ResponsesValidateAndConvertAcrossProviderGroups) {
    using namespace gateway::protocol;

    const auto openai_body = read_fixture("openai_chat_response_v1.json");
    const auto responses_body = read_fixture("openai_responses_response_v1.json");
    const auto anthropic_body = read_fixture("anthropic_messages_response_v1.json");
    const auto gemini_body = read_fixture("gemini_generate_response_v1.json");

    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json; charset=utf-8", openai_body));
    EXPECT_TRUE(Converter::validate_responses_response(responses_body).valid);
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json", anthropic_body));
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json", gemini_body));

    const auto openai_to_anthropic = Converter::convert_response_checked(
        openai_body, Format::OpenAIChatCompletions, Format::Anthropic,
        "claude-sonnet-4-20250514");
    ASSERT_TRUE(openai_to_anthropic.success);
    EXPECT_NE(openai_to_anthropic.body.find("Sunny in Vienna."), std::string::npos);

    const auto anthropic_to_gemini = Converter::convert_response_checked(
        anthropic_body, Format::Anthropic, Format::Gemini, "gemini-2.0-flash");
    ASSERT_TRUE(anthropic_to_gemini.success);
    EXPECT_NE(anthropic_to_gemini.body.find("usageMetadata"), std::string::npos);

    const auto gemini_to_responses = Converter::convert_response_checked(
        gemini_body, Format::Gemini, Format::OpenAIResponses, "gpt-4o");
    ASSERT_TRUE(gemini_to_responses.success);
    EXPECT_TRUE(Converter::validate_responses_response(gemini_to_responses.body).valid);

    const auto errors = read_fixture("provider_errors_v1.json");
    EXPECT_NE(errors.find("\"status\": 400"), std::string::npos);
    EXPECT_NE(errors.find("\"status\": 429"), std::string::npos);
    EXPECT_NE(errors.find("rate_limit_error"), std::string::npos);
}

TEST(ProtocolGolden, StreamingFixturesHaveProviderTerminalEventsAndUsage) {
    using namespace gateway::protocol;

    const auto openai_frames = parse_sse_fixture(read_fixture("openai_chat_stream_v1.sse"));
    ASSERT_EQ(openai_frames.size(), 4u);
    EXPECT_EQ(openai::parse_stream_event(openai_frames[0].data).type,
        StreamDelta::Type::MessageStart);
    auto openai_text = openai::parse_stream_event(openai_frames[1].data);
    EXPECT_EQ(openai_text.type, StreamDelta::Type::TextDelta);
    EXPECT_EQ(openai_text.text, "Sunny in Vienna.");
    EXPECT_EQ(openai::parse_stream_event(openai_frames[2].data).type,
        StreamDelta::Type::MessageEnd);
    EXPECT_EQ(openai::parse_stream_event(openai_frames[3].data).type,
        StreamDelta::Type::Done);

    const auto response_frames = parse_sse_fixture(
        read_fixture("openai_responses_stream_v1.sse"));
    ASSERT_EQ(response_frames.size(), 3u);
    EXPECT_EQ(response_frames[0].event, "response.created");
    EXPECT_EQ(openai_responses::parse_stream_event(response_frames[0].data).type,
        StreamDelta::Type::MessageStart);
    EXPECT_EQ(openai_responses::parse_stream_event(response_frames[1].data).text,
        "Sunny in Vienna.");
    EXPECT_EQ(openai_responses::parse_stream_event(response_frames[2].data).type,
        StreamDelta::Type::Done);

    const auto anthropic_frames = parse_sse_fixture(
        read_fixture("anthropic_messages_stream_v1.sse"));
    ASSERT_EQ(anthropic_frames.size(), 6u);
    auto anthropic_start = anthropic::parse_stream_event(
        anthropic_frames[0].event, anthropic_frames[0].data);
    EXPECT_EQ(anthropic_start.type, StreamDelta::Type::MessageStart);
    EXPECT_EQ(anthropic_start.input_tokens, 12);
    auto anthropic_delta = anthropic::parse_stream_event(
        anthropic_frames[2].event, anthropic_frames[2].data);
    EXPECT_EQ(anthropic_delta.type, StreamDelta::Type::TextDelta);
    EXPECT_EQ(anthropic_delta.text, "Sunny in Vienna.");
    auto anthropic_end = anthropic::parse_stream_event(
        anthropic_frames[4].event, anthropic_frames[4].data);
    EXPECT_EQ(anthropic_end.type, StreamDelta::Type::MessageEnd);
    EXPECT_EQ(anthropic_end.output_tokens, 4);
    EXPECT_EQ(anthropic::parse_stream_event(
        anthropic_frames[5].event, anthropic_frames[5].data).type,
        StreamDelta::Type::Done);

    const auto gemini_frames = parse_sse_fixture(
        read_fixture("gemini_generate_stream_v1.sse"));
    ASSERT_EQ(gemini_frames.size(), 3u);
    auto gemini_text = gemini::parse_stream_event(gemini_frames[0].data);
    EXPECT_EQ(gemini_text.type, StreamDelta::Type::TextDelta);
    EXPECT_EQ(gemini_text.text, "Sunny in Vienna.");
    EXPECT_EQ(gemini::parse_stream_event(gemini_frames[1].data).type,
        StreamDelta::Type::MessageEnd);
    EXPECT_EQ(gemini::parse_stream_event(gemini_frames[2].data).type,
        StreamDelta::Type::Done);
}

TEST(ProtocolGolden, CrossProtocolStreamFixtureConversionPreservesMeaning) {
    using namespace gateway::protocol;
    const auto openai_frames = parse_sse_fixture(read_fixture("openai_chat_stream_v1.sse"));
    const auto gemini_frames = parse_sse_fixture(
        read_fixture("gemini_generate_stream_v1.sse"));

    const auto openai_to_anthropic = Converter::convert_stream_event(
        openai_frames[1].data, Format::OpenAIChatCompletions, Format::Anthropic);
    EXPECT_NE(openai_to_anthropic.find("content_block_delta"), std::string::npos);
    EXPECT_NE(openai_to_anthropic.find("Sunny in Vienna."), std::string::npos);

    const auto gemini_to_openai = Converter::convert_stream_event(
        gemini_frames[0].data, Format::Gemini, Format::OpenAIChatCompletions);
    EXPECT_NE(gemini_to_openai.find("chat.completion.chunk"), std::string::npos);
    EXPECT_NE(gemini_to_openai.find("Sunny in Vienna."), std::string::npos);

    const auto response_to_openai = Converter::convert_stream_event(
        openai_frames[1].data, Format::OpenAIChatCompletions,
        Format::OpenAIResponses);
    EXPECT_NE(response_to_openai.find("response.output_text.delta"), std::string::npos);

}
