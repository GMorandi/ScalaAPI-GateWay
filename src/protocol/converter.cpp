#include "protocol/converter.h"
#include "protocol/formats.h"
#include "platform/logging.h"

#include <simdjson.h>

namespace gateway::protocol {

ParsedRequest Converter::parse(std::string_view body, Format hint) {
    ParsedRequest req;
    req.format = hint;

    ChatRequest ir;
    switch (hint) {
    case Format::Anthropic:
        ir = anthropic::parse_request(body);
        break;
    case Format::OpenAIChatCompletions:
    case Format::OpenAIResponses:
        ir = openai::parse_request(body);
        break;
    case Format::Gemini:
        ir = gemini::parse_request(body);
        break;
    }

    req.model = ir.model;
    req.stream = ir.stream;
    req.max_tokens = ir.max_tokens;
    req.metadata_user_id = ir.metadata_user_id;
    return req;
}

std::string Converter::convert_request(std::string_view body,
                                        Format from, Format to,
                                        const std::string& mapped_model) {
    if (from == to && mapped_model.empty()) {
        return std::string(body);
    }

    ChatRequest ir;
    switch (from) {
    case Format::Anthropic:
        ir = anthropic::parse_request(body);
        break;
    case Format::OpenAIChatCompletions:
    case Format::OpenAIResponses:
        ir = openai::parse_request(body);
        break;
    case Format::Gemini:
        ir = gemini::parse_request(body);
        break;
    }

    if (!mapped_model.empty())
        ir.model = mapped_model;

    switch (to) {
    case Format::Anthropic:
        return anthropic::serialize_request(ir);
    case Format::OpenAIChatCompletions:
    case Format::OpenAIResponses:
        return openai::serialize_request(ir);
    case Format::Gemini:
        return gemini::serialize_request(ir);
    }

    return std::string(body);
}

std::string Converter::convert_stream_event(std::string_view sse_data,
                                             Format from, Format to) {
    if (from == to) {
        return std::string(sse_data);
    }

    StreamDelta delta;
    switch (from) {
    case Format::OpenAIChatCompletions:
    case Format::OpenAIResponses:
        delta = openai::parse_stream_event(sse_data);
        break;
    case Format::Gemini:
        delta = gemini::parse_stream_event(sse_data);
        break;
    case Format::Anthropic:
        break;
    }

    switch (to) {
    case Format::OpenAIChatCompletions:
    case Format::OpenAIResponses:
        return openai::serialize_stream_event(delta);
    case Format::Anthropic:
        return anthropic::serialize_stream_event(delta);
    case Format::Gemini:
        return gemini::serialize_stream_event(delta);
    }

    return std::string(sse_data);
}

}  // namespace gateway::protocol
