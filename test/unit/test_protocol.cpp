#include <gtest/gtest.h>
#include "protocol/formats.h"
#include "protocol/converter.h"
#include "forwarder/forwarder.h"
#include "forwarder/stream_pipe.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace gateway::protocol;

TEST(ProviderResponseValidation, AcceptsCompleteSuccessJson) {
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json; charset=utf-8", R"({"id":"response-1"})"));
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        201, "application/problem+json", R"({"status":"created"})"));
}

TEST(ProviderResponseValidation, RejectsTruncatedOrEmptySuccessJson) {
    EXPECT_TRUE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json", R"({"id":"response-1","choices":[)"));
    EXPECT_TRUE(gateway::forwarder::has_invalid_success_payload(
        200, "application/json", ""));
    EXPECT_TRUE(gateway::forwarder::has_invalid_success_payload(
        200, "", ""));
}

TEST(ProviderResponseValidation, IgnoresNonJsonErrorAndNoContentBodies) {
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        500, "application/json", R"({"truncated":)"));
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        204, "application/json", ""));
    EXPECT_FALSE(gateway::forwarder::has_invalid_success_payload(
        200, "image/png", "not-json"));
}

TEST(ProviderResponseValidation, RequiresExactEventStreamMediaType) {
    EXPECT_TRUE(gateway::forwarder::is_event_stream_content_type(
        "text/event-stream; charset=utf-8"));
    EXPECT_TRUE(gateway::forwarder::is_event_stream_content_type(
        " TEXT/EVENT-STREAM "));
    EXPECT_FALSE(gateway::forwarder::is_event_stream_content_type("application/json"));
    EXPECT_FALSE(gateway::forwarder::is_event_stream_content_type("text/event-streamish"));
    EXPECT_FALSE(gateway::forwarder::is_event_stream_content_type(""));
}

TEST(ProviderResponseEvidence, OnlyExplicitProviderErrorsProveNoCharge) {
    gateway::forwarder::ForwardResult provider_rejection{
        .status_code = 429,
        .provider_response_received = true,
        .provider_status_code = 429,
    };
    EXPECT_TRUE(gateway::forwarder::is_explicit_provider_rejection(provider_rejection));

    auto malformed_success = provider_rejection;
    malformed_success.status_code = 502;
    malformed_success.provider_status_code = 200;
    EXPECT_FALSE(gateway::forwarder::is_explicit_provider_rejection(malformed_success));

    auto transport_failure = provider_rejection;
    transport_failure.status_code = 502;
    transport_failure.provider_response_received = false;
    transport_failure.provider_status_code = 0;
    EXPECT_FALSE(gateway::forwarder::is_explicit_provider_rejection(transport_failure));
}

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

TEST(EmbeddingsValidation, AcceptsStringAndStringArrayInputs) {
    EXPECT_TRUE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":"hello","dimensions":256,"encoding_format":"float","user":"u1"})").valid);
    EXPECT_TRUE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":["hello","world"],"encoding_format":"base64"})").valid);
}

TEST(EmbeddingsValidation, RejectsInvalidInputAndOptions) {
    EXPECT_FALSE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":[]})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":["ok",3]})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":"ok","dimensions":0})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":"ok","encoding_format":"hex"})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_request(
        R"({"model":"text-embedding-3-small","input":"ok","dimensions":8193})").valid);
}

TEST(EmbeddingsValidation, AcceptsMatchingFloatAndBase64Responses) {
    EXPECT_TRUE(Converter::validate_embeddings_response(
        R"({"model":"text-embedding-3-small","input":["hello","world"],"dimensions":3,"encoding_format":"float"})",
        R"({"object":"list","data":[{"object":"embedding","index":0,"embedding":[0.1,0.2,0.3]},{"object":"embedding","index":1,"embedding":[0.4,0.5,0.6]}],"usage":{"prompt_tokens":2,"total_tokens":2}})").valid);
    EXPECT_TRUE(Converter::validate_embeddings_response(
        R"({"model":"text-embedding-3-small","input":"hello","dimensions":2,"encoding_format":"base64"})",
        R"({"object":"list","data":[{"object":"embedding","index":0,"embedding":"AAAAAAAAAAAA"}],"usage":{"prompt_tokens":1,"total_tokens":1}})").valid);
}

TEST(EmbeddingsValidation, RejectsMalformedProviderResponses) {
    const auto request = R"({"model":"text-embedding-3-small","input":["hello","world"],"dimensions":3})";
    EXPECT_FALSE(Converter::validate_embeddings_response(request,
        R"({"data":[{"index":0,"embedding":[0.1,0.2,0.3]}],"usage":{"prompt_tokens":2,"total_tokens":2}})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_response(request,
        R"({"data":[{"index":0,"embedding":[0.1,0.2,0.3]},{"index":1,"embedding":[0.4,null,0.6]}],"usage":{"prompt_tokens":2,"total_tokens":2}})").valid);
    EXPECT_FALSE(Converter::validate_embeddings_response(request,
        R"({"data":[{"index":0,"embedding":[0.1,0.2,0.3]},{"index":1,"embedding":[0.4,0.5,0.6]}],"usage":{"prompt_tokens":0,"total_tokens":0}})").valid);
}

TEST(CatalogValidation, AcceptsOpenAiAndGeminiModelMetadata) {
    EXPECT_TRUE(Converter::validate_models_response(
        R"({"object":"list","data":[{"id":"gpt-4o","object":"model","created":1700000000,"owned_by":"mock"}]})",
        Format::OpenAIChatCompletions).valid);
    EXPECT_TRUE(Converter::validate_models_response(
        R"({"models":[{"name":"models/gemini-2.0-flash","inputTokenLimit":1000000,"outputTokenLimit":8192,"supportedGenerationMethods":["generateContent"]}]})",
        Format::Gemini).valid);
    EXPECT_TRUE(Converter::validate_models_response(
        R"({"name":"models/gemini-2.0-flash","inputTokenLimit":1000000,"outputTokenLimit":8192,"supportedGenerationMethods":["generateContent"]})",
        Format::Gemini).valid);
}

TEST(CatalogValidation, RejectsMalformedOrDuplicateModels) {
    EXPECT_FALSE(Converter::validate_models_response(
        R"({"object":"list","data":[{"id":"gpt-4o","object":"model","created":0,"owned_by":"mock"}]})",
        Format::OpenAIChatCompletions).valid);
    EXPECT_FALSE(Converter::validate_models_response(
        R"({"object":"list","data":[{"id":"gpt-4o","object":"model","created":1,"owned_by":"mock"},{"id":"gpt-4o","object":"model","created":1,"owned_by":"mock"}]})",
        Format::OpenAIChatCompletions).valid);
    EXPECT_FALSE(Converter::validate_models_response(
        R"({"models":[{"name":"gemini-2.0-flash","inputTokenLimit":1,"outputTokenLimit":1,"supportedGenerationMethods":[]}]})",
        Format::Gemini).valid);
}

TEST(CatalogValidation, RequiresBoundedPositiveTokenCount) {
    EXPECT_TRUE(Converter::validate_count_tokens_response(
        R"({"input_tokens":17})").valid);
    EXPECT_FALSE(Converter::validate_count_tokens_response(
        R"({"input_tokens":0})").valid);
    EXPECT_FALSE(Converter::validate_count_tokens_response(
        R"({"input_tokens":"17"})").valid);
}

TEST(ResponsesValidation, AcceptsCompletedUsageEnvelope) {
    EXPECT_TRUE(Converter::validate_responses_response(
        R"({"id":"resp_1","object":"response","status":"completed","model":"gpt-4o","output":[{"type":"message"}],"usage":{"input_tokens":7,"output_tokens":5,"total_tokens":12}})").valid);
}

TEST(ResponsesValidation, RejectsIncompleteOrInconsistentEnvelope) {
    EXPECT_FALSE(Converter::validate_responses_response(
        R"({"id":"resp_1","object":"response","status":"in_progress","model":"gpt-4o","output":[{"type":"message"}],"usage":{"input_tokens":7,"output_tokens":5,"total_tokens":12}})").valid);
    EXPECT_FALSE(Converter::validate_responses_response(
        R"({"id":"resp_1","object":"response","status":"completed","model":"gpt-4o","output":[],"usage":{"input_tokens":7,"output_tokens":5,"total_tokens":12}})").valid);
    EXPECT_FALSE(Converter::validate_responses_response(
        R"({"id":"resp_1","object":"response","status":"completed","model":"gpt-4o","output":[{"type":"message"}],"usage":{"input_tokens":7,"output_tokens":5,"total_tokens":11}})").valid);
}

TEST(RealtimeParse, ExtractsModelFromSessionAndResponseEvents) {
    EXPECT_EQ(Converter::parse_realtime_model(
        R"({"type":"session.update","session":{"model":"gpt-realtime"}})"),
        "gpt-realtime");
    EXPECT_EQ(Converter::parse_realtime_model(
        R"({"type":"response.create","response":{"model":"gpt-realtime-mini"}})"),
        "gpt-realtime-mini");
    EXPECT_TRUE(Converter::parse_realtime_model("not-json").empty());
}

TEST(MultipartParse, ExtractsModelWithoutTouchingFileBytes) {
    const std::string body =
        "--test-boundary\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "gpt-image-1\r\n"
        "--test-boundary\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n"
        "Content-Type: image/png\r\n\r\n"
        "\x89PNG\x00\x01\r\n"
        "--test-boundary--\r\n";
    EXPECT_EQ(Converter::extract_multipart_field(body,
        "multipart/form-data; boundary=\"test-boundary\"", "model"), "gpt-image-1");
}

TEST(MultipartParse, RejectsMissingOrMalformedBoundary) {
    EXPECT_TRUE(Converter::extract_multipart_field("body", "multipart/form-data", "model").empty());
    EXPECT_TRUE(Converter::extract_multipart_field("body",
        "multipart/form-data; boundary=bad\r\nInjected", "model").empty());
}

TEST(MediaUsage, ParsesJsonImageAndVideoBillingFields) {
    auto image = gateway::protocol::Converter::parse_media_request(
        R"({"model":"gpt-image","n":3,"size":"1024x1024"})",
        "application/json", "images_edits");
    EXPECT_EQ(image.input_image_count, 1);
    EXPECT_EQ(image.output_image_count, 3);
    EXPECT_EQ(image.image_size, "1024x1024");

    auto video = gateway::protocol::Converter::parse_media_request(
        R"({"model":"grok-video","duration_seconds":8,"resolution":"1280x720"})",
        "application/json", "videos_generations");
    EXPECT_EQ(video.video_count, 1);
    EXPECT_EQ(video.video_duration_seconds, 8);
    EXPECT_EQ(video.video_resolution, "1280x720");
}

TEST(MediaUsage, ActualProviderOutputCountOverridesRequestedCount) {
    auto response = gateway::protocol::Converter::parse_media_response(
        R"({"data":[{"url":"a"},{"url":"b"}],"size":"512x512"})",
        "images_generations");
    EXPECT_EQ(response.output_image_count, 2);
    EXPECT_EQ(response.image_size, "512x512");
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

TEST(Conversion, CrossProtocolResponseUsesInboundEnvelope) {
    auto result = Converter::convert_response(
        R"({"id":"msg_1","model":"claude","content":[{"type":"text","text":"hello"}],"usage":{"input_tokens":3,"output_tokens":2}})",
        Format::Anthropic, Format::OpenAIResponses, "requested-model");
    EXPECT_NE(result.find("\"object\":\"response\""), std::string::npos);
    EXPECT_NE(result.find("\"output_text\":\"hello\""), std::string::npos);
    EXPECT_NE(result.find("\"input_tokens\":3"), std::string::npos);
    EXPECT_NE(result.find("\"output_tokens\":2"), std::string::npos);
}

TEST(Conversion, CrossProtocolResponseRejectsMalformedJson) {
    auto result = Converter::convert_response_checked(
        "not-json", Format::Anthropic, Format::OpenAIResponses, "model");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.body.empty());
    EXPECT_FALSE(result.error.empty());
}

TEST(Conversion, CrossProtocolResponseRejectsToolCalls) {
    auto result = Converter::convert_response_checked(
        R"({"model":"gpt","choices":[{"message":{"tool_calls":[{"id":"call_1"}]}}]})",
        Format::OpenAIChatCompletions, Format::Anthropic, "claude");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.body.empty());
}

TEST(Conversion, UsageIsMappedToGeminiContract) {
    auto result = Converter::convert_response(
        R"({"model":"gpt","choices":[{"message":{"content":"hello"}}],"usage":{"prompt_tokens":9,"completion_tokens":4,"prompt_tokens_details":{"cached_tokens":3}}})",
        Format::OpenAIChatCompletions, Format::Gemini, "gemini");
    EXPECT_NE(result.find("\"usageMetadata\""), std::string::npos);
    EXPECT_NE(result.find("\"promptTokenCount\":9"), std::string::npos);
    EXPECT_NE(result.find("\"candidatesTokenCount\":4"), std::string::npos);
    EXPECT_NE(result.find("\"cachedContentTokenCount\":3"), std::string::npos);
}

TEST(StreamPipe, ParsesUsageAcrossCrLfChunkBoundaries) {
    std::vector<std::string> chunks = {
        "data: {\"usage\":{\"prompt_tokens\":4,",
        "\"completion_tokens\":2}}\r\n\r\n"
    };
    size_t index = 0;
    std::string emitted;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::OpenAIToAnthropic);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            std::memcpy(out, chunk.data(), std::min(capacity, chunk.size()));
            return static_cast<ssize_t>(chunk.size());
        },
        [&](const char* data, size_t size) -> ssize_t {
            emitted.append(data, size);
            return static_cast<ssize_t>(size);
        });
    EXPECT_EQ(result.input_tokens, 4);
    EXPECT_EQ(result.output_tokens, 2);
    EXPECT_FALSE(emitted.empty());
}

TEST(StreamPipe, ParsesGeminiUsageMetadataAndPreservesRawUsage) {
    std::vector<std::string> chunks = {
        "data: {\"usageMetadata\":{\"promptTokenCount\":7,\"candidatesTokenCount\":3,",
        "\"cachedContentTokenCount\":2,\"thoughtsTokenCount\":1}}\n\n"
    };
    size_t index = 0;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            std::memcpy(out, chunk.data(), std::min(capacity, chunk.size()));
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_EQ(result.input_tokens, 7);
    EXPECT_EQ(result.output_tokens, 3);
    EXPECT_EQ(result.cache_read_tokens, 2);
    EXPECT_EQ(result.reasoning_tokens, 1);
    EXPECT_NE(result.provider_usage_json.find("promptTokenCount"), std::string::npos);
}

TEST(StreamPipe, ParsesAnthropicNestedStartAndFinalUsage) {
    std::vector<std::string> chunks = {
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":29,\"output_tokens\":0}}}\n\n",
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":5}}\n\n"
    };
    size_t index = 0;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            std::memcpy(out, chunk.data(), std::min(capacity, chunk.size()));
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_EQ(result.input_tokens, 29);
    EXPECT_EQ(result.output_tokens, 5);
}

TEST(StreamPipe, RejectsMalformedUsageCounts) {
    const std::string input =
        "data: {\"usage\":{\"prompt_tokens\":-1,\"completion_tokens\":\"invalid\"}}\n\n";
    bool read_once = false;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (read_once) return 0;
            read_once = true;
            std::memcpy(out, input.data(), std::min(capacity, input.size()));
            return static_cast<ssize_t>(input.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_TRUE(result.malformed_usage);
}

TEST(StreamPipe, HandlesPartialWritesDuringTransformation) {
    const std::string input = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n";
    bool read_once = false;
    std::string emitted;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::OpenAIToAnthropic);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (read_once) return 0;
            read_once = true;
            std::memcpy(out, input.data(), std::min(capacity, input.size()));
            return static_cast<ssize_t>(input.size());
        },
        [&](const char* data, size_t size) -> ssize_t {
            const auto written = std::min<size_t>(3, size);
            emitted.append(data, written);
            return static_cast<ssize_t>(written);
        });
    EXPECT_TRUE(result.completed);
    EXPECT_NE(emitted.find("hello"), std::string::npos);
}

TEST(StreamPipe, RequiresTerminalEventBeforeTreatingProviderEofAsComplete) {
    const std::vector<std::string> chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
    };
    size_t index = 0;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough,
                                        Format::OpenAIChatCompletions,
                                        Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(result.incomplete);
    EXPECT_TRUE(result.provider_disconnect);
    EXPECT_FALSE(result.terminal_event_seen);
}

TEST(StreamPipe, TreatsIncompleteChunkedBodyAsProviderDisconnect) {
    const std::string chunk =
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    bool read_once = false;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough,
                                        Format::OpenAIChatCompletions,
                                        Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (read_once) {
                errno = 0; // Photon uses -1/errno=0 for an incomplete chunked body.
                return -1;
            }
            read_once = true;
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_FALSE(result.completed);
    EXPECT_TRUE(result.incomplete);
    EXPECT_TRUE(result.provider_disconnect);
    EXPECT_FALSE(result.terminal_event_seen);
}

TEST(StreamPipe, RecognizesOpenAITerminalEvent) {
    const std::vector<std::string> chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n",
        "data: [DONE]\n\n"
    };
    size_t index = 0;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough,
                                        Format::OpenAIChatCompletions,
                                        Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(result.terminal_event_seen);
    EXPECT_FALSE(result.incomplete);
    EXPECT_FALSE(result.provider_disconnect);
}

TEST(StreamPipe, PreservesUsageFromTruncatedOpenAIStream) {
    const std::vector<std::string> chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n",
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":5,\"total_tokens\":12}}\n\n"
    };
    size_t index = 0;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough,
                                        Format::OpenAIChatCompletions,
                                        Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) {
                errno = ECONNRESET;
                return -1;
            }
            const auto& chunk = chunks[index++];
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });

    EXPECT_TRUE(result.incomplete);
    EXPECT_TRUE(result.provider_disconnect);
    EXPECT_TRUE(result.terminal_event_seen);
    EXPECT_EQ(result.input_tokens, 7);
    EXPECT_EQ(result.output_tokens, 5);
    EXPECT_FALSE(result.provider_usage_json.empty());
}

TEST(StreamPipe, TreatsZeroLengthClientWriteAsDisconnect) {
    bool read_once = false;
    gateway::forwarder::StreamPipe pipe({}, gateway::forwarder::ProtocolMode::Passthrough,
                                        Format::OpenAIChatCompletions,
                                        Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (read_once) return 0;
            read_once = true;
            const std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [](const char*, size_t) -> ssize_t { return 0; });
    EXPECT_TRUE(result.client_disconnect);
    EXPECT_TRUE(result.incomplete);
}

TEST(StreamPipe, EnforcesInterChunkTimeoutAfterFirstToken) {
    gateway::forwarder::StreamPipeConfig config;
    config.first_token_timeout_ms = 100;
    config.inter_chunk_timeout_ms = 5;
    config.total_timeout_ms = 200;
    config.keepalive_interval_ms = 1000;
    gateway::forwarder::StreamPipe pipe(
        config, gateway::forwarder::ProtocolMode::Passthrough,
        Format::OpenAIChatCompletions, Format::OpenAIChatCompletions);

    bool sent_first_chunk = false;
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (!sent_first_chunk) {
                const std::string chunk =
                    "data: {\"choices\":[{\"delta\":{\"content\":\"first\"}}]}\n\n";
                sent_first_chunk = true;
                if (chunk.size() > capacity) return -1;
                std::memcpy(out, chunk.data(), chunk.size());
                return static_cast<ssize_t>(chunk.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            errno = EAGAIN;
            return -1;
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });

    EXPECT_TRUE(result.incomplete);
    EXPECT_TRUE(result.timed_out);
    EXPECT_FALSE(result.provider_disconnect);
    EXPECT_FALSE(result.terminal_event_seen);
    EXPECT_GE(result.first_token_ms, 0);
}

TEST(StreamPipe, EnforcesTotalTimeoutIndependentlyOfInterChunkTimeout) {
    gateway::forwarder::StreamPipeConfig config;
    config.first_token_timeout_ms = 100;
    config.inter_chunk_timeout_ms = 100;
    config.total_timeout_ms = 5;
    config.keepalive_interval_ms = 1000;
    gateway::forwarder::StreamPipe pipe(
        config, gateway::forwarder::ProtocolMode::Passthrough,
        Format::OpenAIChatCompletions, Format::OpenAIChatCompletions);

    bool sent_first_chunk = false;
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (!sent_first_chunk) {
                const std::string chunk =
                    "data: {\"choices\":[{\"delta\":{\"content\":\"first\"}}]}\n\n";
                sent_first_chunk = true;
                if (chunk.size() > capacity) return -1;
                std::memcpy(out, chunk.data(), chunk.size());
                return static_cast<ssize_t>(chunk.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            errno = EAGAIN;
            return -1;
        },
        [](const char*, size_t size) -> ssize_t { return static_cast<ssize_t>(size); });

    EXPECT_TRUE(result.incomplete);
    EXPECT_TRUE(result.timed_out);
    EXPECT_FALSE(result.provider_disconnect);
    EXPECT_FALSE(result.terminal_event_seen);
}

TEST(StreamPipe, WithholdsUnsafeOpenAIEventBeforeClientWrite) {
    gateway::forwarder::StreamPipeConfig config;
    int policy_calls = 0;
    config.policy = [&](std::string_view content) {
        ++policy_calls;
        return content.find("unsafe") != std::string_view::npos
            ? gateway::forwarder::StreamPolicyDecision::Blocked(
                "content_policy_blocked", "blocked stream event")
            : gateway::forwarder::StreamPolicyDecision::Allowed();
    };
    const std::vector<std::string> chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"safe\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"unsafe\"}}]}\n\n",
        "data: [DONE]\n\n"
    };
    size_t index = 0;
    std::string emitted;
    gateway::forwarder::StreamPipe pipe(
        config, gateway::forwarder::ProtocolMode::Passthrough,
        Format::OpenAIChatCompletions, Format::OpenAIChatCompletions);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (index == chunks.size()) return 0;
            const auto& chunk = chunks[index++];
            if (chunk.size() > capacity) return -1;
            std::memcpy(out, chunk.data(), chunk.size());
            return static_cast<ssize_t>(chunk.size());
        },
        [&](const char* data, size_t size) -> ssize_t {
            emitted.append(data, size);
            return static_cast<ssize_t>(size);
        });

    EXPECT_TRUE(result.policy_blocked);
    EXPECT_FALSE(result.policy_failed_closed);
    EXPECT_TRUE(result.incomplete);
    EXPECT_EQ(result.policy_error_code, "content_policy_blocked");
    EXPECT_EQ(policy_calls, 2);
    EXPECT_NE(emitted.find("safe"), std::string::npos);
    EXPECT_EQ(emitted.find("unsafe"), std::string::npos);
    EXPECT_NE(emitted.find("content_policy_blocked"), std::string::npos);
}

TEST(StreamPipe, EmitsAnthropicPolicyFailureAndFailsClosedOnOversizedEvent) {
    gateway::forwarder::StreamPipeConfig config;
    config.max_policy_event_bytes = 16;
    int policy_calls = 0;
    config.policy = [&](std::string_view) {
        ++policy_calls;
        return gateway::forwarder::StreamPolicyDecision::Allowed();
    };
    const std::string input =
        "event: content_block_delta\ndata: {\"text\":\"bounded\"}\n\n";
    bool read_once = false;
    std::string emitted;
    gateway::forwarder::StreamPipe pipe(
        config, gateway::forwarder::ProtocolMode::Passthrough,
        Format::Anthropic, Format::Anthropic);
    auto result = pipe.run(
        [&](char* out, size_t capacity) -> ssize_t {
            if (read_once) return 0;
            read_once = true;
            if (input.size() > capacity) return -1;
            std::memcpy(out, input.data(), input.size());
            return static_cast<ssize_t>(input.size());
        },
        [&](const char* data, size_t size) -> ssize_t {
            emitted.append(data, size);
            return static_cast<ssize_t>(size);
        });

    EXPECT_TRUE(result.policy_failed_closed);
    EXPECT_FALSE(result.policy_blocked);
    EXPECT_EQ(result.policy_error_code, "content_policy_payload_too_large");
    EXPECT_EQ(policy_calls, 0);
    EXPECT_NE(emitted.find("event: error"), std::string::npos);
    EXPECT_NE(emitted.find("content_policy_payload_too_large"), std::string::npos);
    EXPECT_EQ(emitted.find("bounded"), std::string::npos);
}

TEST(StreamPipe, EmitsOpenAIResponsesAndGeminiPolicyErrorEvents) {
    const auto run = [](Format format) {
        gateway::forwarder::StreamPipeConfig config;
        config.policy = [](std::string_view) {
            return gateway::forwarder::StreamPolicyDecision::FailedClosed(
                "content_policy_unavailable", "classifier unavailable");
        };
        const std::string input = "data: {\"text\":\"unsafe\"}\n\n";
        bool read_once = false;
        std::string emitted;
        gateway::forwarder::StreamPipe pipe(
            config, gateway::forwarder::ProtocolMode::Passthrough,
            format, format);
        auto result = pipe.run(
            [&](char* out, size_t capacity) -> ssize_t {
                if (read_once) return 0;
                read_once = true;
                if (input.size() > capacity) return -1;
                std::memcpy(out, input.data(), input.size());
                return static_cast<ssize_t>(input.size());
            },
            [&](const char* data, size_t size) -> ssize_t {
                emitted.append(data, size);
                return static_cast<ssize_t>(size);
            });
        return std::pair{result, emitted};
    };

    const auto responses = run(Format::OpenAIResponses);
    EXPECT_TRUE(responses.first.policy_failed_closed);
    EXPECT_NE(responses.second.find("event: response.failed"), std::string::npos);
    EXPECT_NE(responses.second.find("content_policy_unavailable"), std::string::npos);

    const auto gemini = run(Format::Gemini);
    EXPECT_TRUE(gemini.first.policy_failed_closed);
    EXPECT_NE(gemini.second.find("data: {\"error\""), std::string::npos);
    EXPECT_NE(gemini.second.find("classifier unavailable"), std::string::npos);
}
