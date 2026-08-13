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

struct ValidationResult {
    bool valid = false;
    std::string message;
};

struct ResponseConversionResult {
    bool success = false;
    std::string body;
    std::string error;
};

struct MediaUsageMetadata {
    int input_image_count = 0;
    int output_image_count = 0;
    std::string image_size;
    int video_count = 0;
    std::string video_resolution;
    int video_duration_seconds = 0;
};

// Unknown Field Policy:
// All request and response parsers silently ignore unrecognized fields.
// Extra JSON keys in any input document are skipped without error. This
// ensures forward-compatibility: when a provider adds new fields, the
// gateway will not crash or produce malformed output. Parsers extract only
// the fields they need and discard everything else. Tests in
// test_protocol.cpp verify this behaviour for each format.

class Converter {
public:
    static ParsedRequest parse(std::string_view body, Format hint);

    static std::string convert_request(std::string_view body,
                                        Format from, Format to,
                                        const std::string& mapped_model);

    static std::string convert_response(std::string_view body,
                                        Format from, Format to,
                                        std::string_view requested_model = {});

    // Normalize a provider error into the target protocol envelope. Errors
    // already using the requested protocol are returned unchanged so that
    // provider-specific fields remain available to that client.
    static std::string convert_error(std::string_view body, int status_code,
                                     Format from, Format to);

    static ResponseConversionResult convert_response_checked(
        std::string_view body, Format from, Format to,
        std::string_view requested_model = {});

    static std::string convert_stream_event(std::string_view sse_data,
                                             Format from, Format to);

    static ValidationResult validate_embeddings_request(std::string_view body);
    static ValidationResult validate_embeddings_response(
        std::string_view request_body, std::string_view response_body);
    static ValidationResult validate_models_response(
        std::string_view response_body, Format format);
    static ValidationResult validate_count_tokens_response(
        std::string_view response_body);
    static ValidationResult validate_responses_response(
        std::string_view response_body);
    static std::string parse_realtime_model(std::string_view event);
    static std::string extract_multipart_field(std::string_view body,
                                               std::string_view content_type,
                                               std::string_view field_name);
    static MediaUsageMetadata parse_media_request(std::string_view body,
                                                  std::string_view content_type,
                                                  std::string_view operation);
    static MediaUsageMetadata parse_media_response(std::string_view body,
                                                   std::string_view operation);
};

}  // namespace gateway::protocol
