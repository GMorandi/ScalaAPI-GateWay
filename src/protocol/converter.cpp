#include "protocol/converter.h"
#include "platform/logging.h"

#include <simdjson.h>

namespace gateway::protocol {

ParsedRequest Converter::parse(std::string_view body, Format hint) {
    ParsedRequest req;
    req.format = hint;

    simdjson::padded_string padded(body);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded);

    if (auto model = doc["model"].get_string(); model.error() == simdjson::SUCCESS) {
        req.model = std::string(model.value());
    }
    if (auto stream = doc["stream"].get_bool(); stream.error() == simdjson::SUCCESS) {
        req.stream = stream.value();
    }
    if (auto max_tok = doc["max_tokens"].get_int64(); max_tok.error() == simdjson::SUCCESS) {
        req.max_tokens = static_cast<int>(max_tok.value());
    }

    return req;
}

std::string Converter::convert_request(std::string_view body,
                                        Format from, Format to,
                                        const std::string& mapped_model) {
    if (from == to) {
        return std::string(body);
    }
    // Protocol conversion logic:
    // OpenAI CC -> Anthropic: messages array transform, system extraction
    // Anthropic -> OpenAI CC: content blocks -> choices
    // Gemini <-> Anthropic: parts/contents <-> messages
    return std::string(body);
}

std::string Converter::convert_stream_event(std::string_view sse_data,
                                             Format from, Format to) {
    if (from == to) {
        return std::string(sse_data);
    }
    // SSE event transformation for streaming
    return std::string(sse_data);
}

}  // namespace gateway::protocol
