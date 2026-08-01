#pragma once

#include <string_view>
#include <string>

namespace gateway::protocol {

enum class Format {
    Anthropic,
    OpenAIChatCompletions,
    OpenAIResponses,
    Gemini,
};

struct ParsedRequest {
    Format format;
    std::string model;
    bool stream = false;
    std::string metadata_user_id;
    int max_tokens = 0;
    bool thinking_enabled = false;
};

class Converter {
public:
    static ParsedRequest parse(std::string_view body, Format hint);

    static std::string convert_request(std::string_view body,
                                        Format from, Format to,
                                        const std::string& mapped_model);

    static std::string convert_stream_event(std::string_view sse_data,
                                             Format from, Format to);
};

}  // namespace gateway::protocol
