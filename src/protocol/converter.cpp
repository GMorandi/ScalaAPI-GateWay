#include "protocol/converter.h"
#include "protocol/formats.h"
#include "platform/logging.h"

#include <simdjson.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <charconv>
#include <unordered_set>

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
        ir = openai::parse_request(body);
        break;
    case Format::OpenAIResponses:
        ir = openai_responses::parse_request(body);
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

ValidationResult Converter::validate_embeddings_request(std::string_view body) {
    constexpr size_t max_inputs = 2048;
    constexpr int max_dimensions = 8192;
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject())
        return {false, "Request body must be a JSON object"};
    if (!document.HasMember("model") || !document["model"].IsString()
        || document["model"].GetStringLength() == 0)
        return {false, "model must be a non-empty string"};
    if (!document.HasMember("input"))
        return {false, "input is required"};
    const auto& input = document["input"];
    if (input.IsString()) {
        if (input.GetStringLength() == 0) return {false, "input must not be empty"};
    } else if (input.IsArray()) {
        if (input.Empty()) return {false, "input must not be an empty array"};
        if (input.Size() > max_inputs)
            return {false, "input must contain at most 2048 strings"};
        for (const auto& value : input.GetArray())
            if (!value.IsString() || value.GetStringLength() == 0)
                return {false, "input array entries must be non-empty strings"};
    } else {
        return {false, "input must be a string or an array of strings"};
    }
    if (document.HasMember("encoding_format")) {
        const auto& encoding = document["encoding_format"];
        if (!encoding.IsString()
            || (std::string_view(encoding.GetString(), encoding.GetStringLength()) != "float"
                && std::string_view(encoding.GetString(), encoding.GetStringLength()) != "base64"))
            return {false, "encoding_format must be float or base64"};
    }
    int max_dimensions_for_model = max_dimensions;
    const std::string_view model(document["model"].GetString(),
                                 document["model"].GetStringLength());
    if (model == "jina-embeddings-v5-text-small")
        max_dimensions_for_model = 1024;
    else if (model == "gemini-embedding-001")
        max_dimensions_for_model = 3072;

    if (document.HasMember("dimensions")) {
        const auto& dimensions = document["dimensions"];
        if (!dimensions.IsInt() || dimensions.GetInt() <= 0)
            return {false, "dimensions must be a positive integer"};
        if (dimensions.GetInt() > max_dimensions_for_model)
            return {false, "dimensions exceed the model profile maximum"};
    }
    if (document.HasMember("user") && !document["user"].IsString())
        return {false, "user must be a string"};
    return {true, {}};
}

ValidationResult Converter::validate_embeddings_response(
    std::string_view request_body, std::string_view response_body) {
    rapidjson::Document request;
    request.Parse(request_body.data(), request_body.size());
    if (request.HasParseError() || !request.IsObject() || !request.HasMember("input"))
        return {false, "Embedding request could not be inspected"};

    const auto& input = request["input"];
    const size_t expected_count = input.IsArray() ? input.Size() : 1;
    int expected_dimensions = 0;
    if (request.HasMember("dimensions") && request["dimensions"].IsInt())
        expected_dimensions = request["dimensions"].GetInt();
    std::string_view encoding = "float";
    if (request.HasMember("encoding_format") && request["encoding_format"].IsString())
        encoding = {request["encoding_format"].GetString(),
                    request["encoding_format"].GetStringLength()};

    rapidjson::Document response;
    response.Parse(response_body.data(), response_body.size());
    if (response.HasParseError() || !response.IsObject())
        return {false, "Provider returned an invalid embeddings JSON object"};
    if (!response.HasMember("data") || !response["data"].IsArray())
        return {false, "Provider embeddings response is missing data"};
    const auto& data = response["data"];
    if (data.Size() != expected_count)
        return {false, "Provider returned an unexpected embeddings count"};

    for (rapidjson::SizeType i = 0; i < data.Size(); ++i) {
        const auto& item = data[i];
        if (!item.IsObject() || !item.HasMember("index") || !item["index"].IsInt()
            || item["index"].GetInt() != static_cast<int>(i)
            || !item.HasMember("embedding"))
            return {false, "Provider returned a malformed embedding item"};
        const auto& embedding = item["embedding"];
        if (encoding == "base64") {
            if (!embedding.IsString() || embedding.GetStringLength() == 0)
                return {false, "Provider returned a non-base64 embedding"};
            if (expected_dimensions > 0) {
                const auto expected_bytes = static_cast<size_t>(expected_dimensions) * 4;
                const auto expected_length = ((expected_bytes + 2) / 3) * 4;
                if (embedding.GetStringLength() != expected_length)
                    return {false, "Provider returned an unexpected base64 embedding dimension"};
            }
        } else {
            if (!embedding.IsArray() || embedding.Empty())
                return {false, "Provider returned a non-float embedding"};
            if (expected_dimensions > 0 && embedding.Size() != static_cast<size_t>(expected_dimensions))
                return {false, "Provider returned an unexpected embedding dimension"};
            for (const auto& value : embedding.GetArray())
                if (!value.IsNumber() || !std::isfinite(value.GetDouble()))
                    return {false, "Provider returned a non-finite embedding value"};
        }
    }
    if (!response.HasMember("usage") || !response["usage"].IsObject()
        || !response["usage"].HasMember("prompt_tokens")
        || !response["usage"]["prompt_tokens"].IsInt()
        || response["usage"]["prompt_tokens"].GetInt() <= 0
        || !response["usage"].HasMember("total_tokens")
        || !response["usage"]["total_tokens"].IsInt()
        || response["usage"]["total_tokens"].GetInt() <= 0)
        return {false, "Provider embeddings response is missing positive usage"};
    return {true, {}};
}

ValidationResult Converter::validate_models_response(
    std::string_view response_body, Format format) {
    rapidjson::Document response;
    response.Parse(response_body.data(), response_body.size());
    if (response.HasParseError() || !response.IsObject())
        return {false, "Provider models response must be a JSON object"};

    if (format == Format::Gemini) {
        auto validate_model = [](const rapidjson::Value& model) -> ValidationResult {
            if (!model.IsObject() || !model.HasMember("name") || !model["name"].IsString()
                || model["name"].GetStringLength() <= std::string_view("models/").size()
                || std::string_view(model["name"].GetString(), model["name"].GetStringLength())
                    .substr(0, std::string_view("models/").size()) != "models/"
                || !model.HasMember("supportedGenerationMethods")
                || !model["supportedGenerationMethods"].IsArray()
                || model["supportedGenerationMethods"].Empty())
                return {false, "Provider Gemini model metadata is incomplete"};
            for (const auto& method : model["supportedGenerationMethods"].GetArray())
                if (!method.IsString() || method.GetStringLength() == 0)
                    return {false, "Provider Gemini model methods are malformed"};
            for (const char* limit : {"inputTokenLimit", "outputTokenLimit"})
                if (!model.HasMember(limit) || !model[limit].IsInt64() || model[limit].GetInt64() <= 0)
                    return {false, "Provider Gemini model token limits are malformed"};
            return {true, {}};
        };

        if (response.HasMember("models")) {
            if (!response["models"].IsArray())
                return {false, "Provider Gemini models must be an array"};
            for (const auto& model : response["models"].GetArray()) {
                auto result = validate_model(model);
                if (!result.valid) return result;
            }
            return {true, {}};
        }
        return validate_model(response);
    }

    if (!response.HasMember("object") || !response["object"].IsString()
        || std::string_view(response["object"].GetString(), response["object"].GetStringLength()) != "list"
        || !response.HasMember("data") || !response["data"].IsArray())
        return {false, "Provider models response must contain a list and data array"};

    std::unordered_set<std::string_view> ids;
    for (const auto& model : response["data"].GetArray()) {
        if (!model.IsObject() || !model.HasMember("id") || !model["id"].IsString()
            || model["id"].GetStringLength() == 0
            || !model.HasMember("object") || !model["object"].IsString()
            || std::string_view(model["object"].GetString(), model["object"].GetStringLength()) != "model"
            || !model.HasMember("created") || !model["created"].IsInt64()
            || model["created"].GetInt64() <= 0
            || !model.HasMember("owned_by") || !model["owned_by"].IsString()
            || model["owned_by"].GetStringLength() == 0)
            return {false, "Provider model metadata is incomplete"};
        const std::string_view id(model["id"].GetString(), model["id"].GetStringLength());
        if (!ids.emplace(id).second)
            return {false, "Provider model catalog contains duplicate ids"};
    }
    return {true, {}};
}

ValidationResult Converter::validate_count_tokens_response(std::string_view response_body) {
    rapidjson::Document response;
    response.Parse(response_body.data(), response_body.size());
    if (response.HasParseError() || !response.IsObject()
        || !response.HasMember("input_tokens") || !response["input_tokens"].IsInt64()
        || response["input_tokens"].GetInt64() <= 0
        || response["input_tokens"].GetInt64() > 1'000'000'000)
        return {false, "Provider token-count response must contain a bounded positive input_tokens value"};
    return {true, {}};
}

ValidationResult Converter::validate_responses_response(std::string_view response_body) {
    rapidjson::Document response;
    response.Parse(response_body.data(), response_body.size());
    if (response.HasParseError() || !response.IsObject())
        return {false, "Provider Responses response must be a JSON object"};
    if (!response.HasMember("id") || !response["id"].IsString()
        || response["id"].GetStringLength() == 0
        || !response.HasMember("object") || !response["object"].IsString()
        || std::string_view(response["object"].GetString(), response["object"].GetStringLength()) != "response"
        || !response.HasMember("status") || !response["status"].IsString()
        || std::string_view(response["status"].GetString(), response["status"].GetStringLength()) != "completed"
        || !response.HasMember("model") || !response["model"].IsString()
        || response["model"].GetStringLength() == 0)
        return {false, "Provider Responses response metadata is incomplete"};
    if (!response.HasMember("output") || !response["output"].IsArray()
        || response["output"].Empty())
        return {false, "Provider Responses response is missing output"};
    for (const auto& item : response["output"].GetArray())
        if (!item.IsObject() || !item.HasMember("type") || !item["type"].IsString()
            || item["type"].GetStringLength() == 0)
            return {false, "Provider Responses output item is malformed"};
    if (!response.HasMember("usage") || !response["usage"].IsObject())
        return {false, "Provider Responses response is missing usage"};
    const auto& usage = response["usage"];
    if (!usage.HasMember("input_tokens") || !usage["input_tokens"].IsInt64()
        || usage["input_tokens"].GetInt64() <= 0
        || !usage.HasMember("output_tokens") || !usage["output_tokens"].IsInt64()
        || usage["output_tokens"].GetInt64() <= 0
        || !usage.HasMember("total_tokens") || !usage["total_tokens"].IsInt64()
        || usage["total_tokens"].GetInt64() < usage["input_tokens"].GetInt64() + usage["output_tokens"].GetInt64())
        return {false, "Provider Responses usage is incomplete or inconsistent"};
    return {true, {}};
}

std::string Converter::parse_realtime_model(std::string_view event) {
    rapidjson::Document document;
    document.Parse(event.data(), event.size());
    if (document.HasParseError() || !document.IsObject()) return {};
    auto model_from = [](const rapidjson::Value& value) -> std::string {
        return value.IsObject() && value.HasMember("model") && value["model"].IsString()
            ? std::string(value["model"].GetString(), value["model"].GetStringLength())
            : std::string{};
    };
    auto model = model_from(document);
    if (!model.empty()) return model;
    for (const char* container : {"session", "response"}) {
        if (document.HasMember(container)) {
            model = model_from(document[container]);
            if (!model.empty()) return model;
        }
    }
    return {};
}

std::string Converter::extract_multipart_field(std::string_view body,
                                                std::string_view content_type,
                                                std::string_view field_name) {
    std::string lowered(content_type);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto marker = lowered.find("boundary=");
    if (marker == std::string::npos) return {};
    auto start = marker + std::string_view("boundary=").size();
    while (start < content_type.size() && content_type[start] == ' ') ++start;
    auto end = content_type.find(';', start);
    if (end == std::string_view::npos) end = content_type.size();
    auto boundary = content_type.substr(start, end - start);
    while (!boundary.empty() && boundary.back() == ' ') boundary.remove_suffix(1);
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary.remove_prefix(1);
        boundary.remove_suffix(1);
    }
    if (boundary.empty() || boundary.size() > 200
        || boundary.find('\r') != std::string_view::npos
        || boundary.find('\n') != std::string_view::npos) return {};

    const std::string delimiter = "--" + std::string(boundary);
    size_t position = 0;
    while ((position = body.find(delimiter, position)) != std::string_view::npos) {
        position += delimiter.size();
        if (body.substr(position, 2) == "--") break;
        if (body.substr(position, 2) != "\r\n") continue;
        position += 2;
        auto headers_end = body.find("\r\n\r\n", position);
        if (headers_end == std::string_view::npos) return {};
        auto headers = body.substr(position, headers_end - position);
        std::string lowered_headers(headers);
        std::transform(lowered_headers.begin(), lowered_headers.end(), lowered_headers.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string name_marker = "name=\"" + std::string(field_name) + "\"";
        auto next = body.find("\r\n" + delimiter, headers_end + 4);
        if (next == std::string_view::npos) return {};
        if (lowered_headers.find("content-disposition: form-data") != std::string::npos
            && lowered_headers.find(name_marker) != std::string::npos) {
            auto value = body.substr(headers_end + 4, next - (headers_end + 4));
            if (value.size() > 1024) return {};
            return std::string(value);
        }
        position = next + 2;
    }
    return {};
}

namespace {

int positive_integer(const rapidjson::Value& object,
                     std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (!object.IsObject() || !object.HasMember(key)) continue;
        const auto& value = object[key];
        if (value.IsInt()) return std::max(0, value.GetInt());
        if (value.IsUint()) return static_cast<int>(std::min<unsigned>(
            value.GetUint(), std::numeric_limits<int>::max()));
        if (value.IsString()) {
            int parsed = 0;
            auto text = std::string_view(value.GetString(), value.GetStringLength());
            auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
                return std::max(0, parsed);
        }
    }
    return 0;
}

std::string string_member(const rapidjson::Value& object,
                          std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (object.IsObject() && object.HasMember(key) && object[key].IsString())
            return object[key].GetString();
    }
    return {};
}

int multipart_integer(std::string_view body, std::string_view content_type,
                      std::initializer_list<std::string_view> fields) {
    for (auto field : fields) {
        auto value = Converter::extract_multipart_field(body, content_type, field);
        if (value.empty()) continue;
        int parsed = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == value.data() + value.size())
            return std::max(0, parsed);
    }
    return 0;
}

}  // namespace

MediaUsageMetadata Converter::parse_media_request(
    std::string_view body, std::string_view content_type,
    std::string_view operation) {
    MediaUsageMetadata usage;
    const bool image = operation.starts_with("images_");
    const bool video = operation.starts_with("videos_");
    const bool edit = operation.find("edits") != std::string_view::npos;
    if (image && edit) usage.input_image_count = 1;
    if (video) usage.video_count = 1;

    if (content_type.starts_with("multipart/form-data")) {
        usage.output_image_count = image
            ? std::max(1, multipart_integer(body, content_type, {"n"})) : 0;
        usage.image_size = extract_multipart_field(body, content_type, "size");
        usage.video_resolution = extract_multipart_field(body, content_type, "resolution");
        usage.video_duration_seconds = multipart_integer(
            body, content_type, {"duration", "duration_seconds"});
        return usage;
    }

    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject()) return usage;
    usage.output_image_count = image
        ? std::max(1, positive_integer(document, {"n"})) : 0;
    usage.image_size = string_member(document, {"size"});
    usage.video_resolution = string_member(document, {"resolution"});
    usage.video_duration_seconds = positive_integer(
        document, {"duration", "duration_seconds"});
    return usage;
}

MediaUsageMetadata Converter::parse_media_response(
    std::string_view body, std::string_view operation) {
    MediaUsageMetadata usage;
    rapidjson::Document document;
    document.Parse(body.data(), body.size());
    if (document.HasParseError() || !document.IsObject()) return usage;
    if (operation.starts_with("images_")) {
        if (document.HasMember("data") && document["data"].IsArray())
            usage.output_image_count = static_cast<int>(document["data"].Size());
        else
            usage.output_image_count = positive_integer(document, {"output_count", "n"});
        usage.image_size = string_member(document, {"size"});
    }
    if (operation.starts_with("videos_")) {
        usage.video_count = 1;
        usage.video_resolution = string_member(document, {"resolution"});
        usage.video_duration_seconds = positive_integer(
            document, {"duration", "duration_seconds"});
    }
    return usage;
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
        ir = openai::parse_request(body);
        break;
    case Format::OpenAIResponses:
        ir = openai_responses::parse_request(body);
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
        return openai::serialize_request(ir);
    case Format::OpenAIResponses:
        return openai_responses::serialize_request(ir);
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
        delta = openai::parse_stream_event(sse_data);
        break;
    case Format::OpenAIResponses:
        delta = openai_responses::parse_stream_event(sse_data);
        break;
    case Format::Gemini:
        delta = gemini::parse_stream_event(sse_data);
        break;
    case Format::Anthropic:
        break;
    }

    switch (to) {
    case Format::OpenAIChatCompletions:
        return openai::serialize_stream_event(delta);
    case Format::OpenAIResponses:
        return openai_responses::serialize_stream_event(delta);
    case Format::Anthropic:
        return anthropic::serialize_stream_event(delta);
    case Format::Gemini:
        return gemini::serialize_stream_event(delta);
    }

    return std::string(sse_data);
}

namespace {
namespace rj = rapidjson;

std::string member_string(const rj::Value& value, const char* key) {
    return value.IsObject() && value.HasMember(key) && value[key].IsString()
        ? std::string(value[key].GetString()) : std::string{};
}

FinishReason parse_finish_reason(Format from, const rj::Value& root) {
    if (from == Format::OpenAIChatCompletions) {
        if (root.HasMember("choices") && root["choices"].IsArray() && !root["choices"].Empty()) {
            const auto& choice = root["choices"][0];
            if (choice.IsObject() && choice.HasMember("finish_reason") && choice["finish_reason"].IsString()) {
                std::string_view reason(choice["finish_reason"].GetString(), choice["finish_reason"].GetStringLength());
                if (reason == "stop") return FinishReason::Stop;
                if (reason == "length") return FinishReason::Length;
                if (reason == "tool_calls") return FinishReason::ToolCalls;
                if (reason == "content_filter") return FinishReason::ContentFilter;
            }
        }
    } else if (from == Format::Anthropic) {
        if (root.HasMember("stop_reason") && root["stop_reason"].IsString()) {
            std::string_view reason(root["stop_reason"].GetString(), root["stop_reason"].GetStringLength());
            if (reason == "end_turn") return FinishReason::Stop;
            if (reason == "max_tokens") return FinishReason::Length;
            if (reason == "tool_use") return FinishReason::ToolCalls;
        }
    } else if (from == Format::Gemini) {
        if (root.HasMember("candidates") && root["candidates"].IsArray() && !root["candidates"].Empty()) {
            const auto& candidate = root["candidates"][0];
            if (candidate.IsObject() && candidate.HasMember("finishReason") && candidate["finishReason"].IsString()) {
                std::string_view reason(candidate["finishReason"].GetString(), candidate["finishReason"].GetStringLength());
                if (reason == "STOP") {
                    // Gemini uses STOP for both normal stops and tool calls; detect from content
                    if (candidate.HasMember("content") && candidate["content"].IsObject()) {
                        const auto& content = candidate["content"];
                        if (content.HasMember("parts") && content["parts"].IsArray()) {
                            for (const auto& part : content["parts"].GetArray()) {
                                if (part.IsObject() && part.HasMember("functionCall"))
                                    return FinishReason::ToolCalls;
                            }
                        }
                    }
                    return FinishReason::Stop;
                }
                if (reason == "MAX_TOKENS") return FinishReason::Length;
                if (reason == "SAFETY") return FinishReason::Safety;
                if (reason == "RECITATION") return FinishReason::Recitation;
            }
        }
    } else if (from == Format::OpenAIResponses) {
        if (root.HasMember("status") && root["status"].IsString()) {
            std::string_view status(root["status"].GetString(), root["status"].GetStringLength());
            if (status == "completed") {
                if (root.HasMember("output") && root["output"].IsArray()) {
                    for (const auto& item : root["output"].GetArray()) {
                        if (item.IsObject() && item.HasMember("type") && item["type"].IsString()) {
                            std::string_view type(item["type"].GetString(), item["type"].GetStringLength());
                            if (type == "function_call") return FinishReason::ToolCalls;
                        }
                    }
                }
                return FinishReason::Stop;
            }
            if (status == "incomplete") return FinishReason::Length;
            if (status == "failed") return FinishReason::ContentFilter;
        }
    }
    return FinishReason::Unknown;
}

std::string serialize_finish_reason(FinishReason reason, Format to) {
    switch (to) {
    case Format::OpenAIChatCompletions:
        switch (reason) {
        case FinishReason::Stop: return "stop";
        case FinishReason::Length: return "length";
        case FinishReason::ToolCalls: return "tool_calls";
        case FinishReason::ContentFilter: return "content_filter";
        case FinishReason::Safety: return "content_filter";
        case FinishReason::Recitation: return "content_filter";
        case FinishReason::Unknown: return "stop";
        }
        break;
    case Format::Anthropic:
        switch (reason) {
        case FinishReason::Stop: return "end_turn";
        case FinishReason::Length: return "max_tokens";
        case FinishReason::ToolCalls: return "tool_use";
        case FinishReason::ContentFilter: return "end_turn";
        case FinishReason::Safety: return "end_turn";
        case FinishReason::Recitation: return "end_turn";
        case FinishReason::Unknown: return "end_turn";
        }
        break;
    case Format::Gemini:
        switch (reason) {
        case FinishReason::Stop: return "STOP";
        case FinishReason::Length: return "MAX_TOKENS";
        case FinishReason::ToolCalls: return "STOP";
        case FinishReason::ContentFilter: return "SAFETY";
        case FinishReason::Safety: return "SAFETY";
        case FinishReason::Recitation: return "RECITATION";
        case FinishReason::Unknown: return "STOP";
        }
        break;
    case Format::OpenAIResponses:
        switch (reason) {
        case FinishReason::Stop: return "completed";
        case FinishReason::Length: return "incomplete";
        case FinishReason::ToolCalls: return "completed";
        case FinishReason::ContentFilter: return "failed";
        case FinishReason::Safety: return "failed";
        case FinishReason::Recitation: return "failed";
        case FinishReason::Unknown: return "completed";
        }
        break;
    }
    return "stop";
}

struct ExtractedToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

std::vector<ExtractedToolCall> extract_tool_calls(Format from, const rj::Value& root) {
    std::vector<ExtractedToolCall> calls;
    if (from == Format::OpenAIChatCompletions) {
        if (root.HasMember("choices") && root["choices"].IsArray() && !root["choices"].Empty()) {
            const auto& choice = root["choices"][0];
            if (choice.IsObject() && choice.HasMember("message") && choice["message"].IsObject()) {
                const auto& message = choice["message"];
                if (message.HasMember("tool_calls") && message["tool_calls"].IsArray()) {
                    for (const auto& tc : message["tool_calls"].GetArray()) {
                        if (!tc.IsObject()) continue;
                        ExtractedToolCall call;
                        call.id = member_string(tc, "id");
                        if (tc.HasMember("function") && tc["function"].IsObject()) {
                            call.name = member_string(tc["function"], "name");
                            call.arguments = member_string(tc["function"], "arguments");
                        }
                        calls.push_back(std::move(call));
                    }
                }
            }
        }
    } else if (from == Format::Anthropic) {
        if (root.HasMember("content") && root["content"].IsArray()) {
            for (const auto& block : root["content"].GetArray()) {
                if (!block.IsObject() || !block.HasMember("type") || !block["type"].IsString()) continue;
                std::string_view type(block["type"].GetString(), block["type"].GetStringLength());
                if (type != "tool_use") continue;
                ExtractedToolCall call;
                call.id = member_string(block, "id");
                call.name = member_string(block, "name");
                if (block.HasMember("input") && block["input"].IsObject()) {
                    rj::StringBuffer sb;
                    rj::Writer<rj::StringBuffer> w(sb);
                    block["input"].Accept(w);
                    call.arguments = sb.GetString();
                }
                calls.push_back(std::move(call));
            }
        }
    } else if (from == Format::Gemini) {
        if (root.HasMember("candidates") && root["candidates"].IsArray() && !root["candidates"].Empty()) {
            const auto& candidate = root["candidates"][0];
            if (!candidate.IsObject() || !candidate.HasMember("content") || !candidate["content"].IsObject()) return calls;
            const auto& content = candidate["content"];
            if (!content.HasMember("parts") || !content["parts"].IsArray()) return calls;
            for (const auto& part : content["parts"].GetArray()) {
                if (!part.IsObject() || !part.HasMember("functionCall") || !part["functionCall"].IsObject()) continue;
                const auto& fc = part["functionCall"];
                ExtractedToolCall call;
                call.name = member_string(fc, "name");
                call.id = call.name;
                if (fc.HasMember("args") && fc["args"].IsObject()) {
                    rj::StringBuffer sb;
                    rj::Writer<rj::StringBuffer> w(sb);
                    fc["args"].Accept(w);
                    call.arguments = sb.GetString();
                }
                calls.push_back(std::move(call));
            }
        }
    } else if (from == Format::OpenAIResponses) {
        if (root.HasMember("output") && root["output"].IsArray()) {
            for (const auto& item : root["output"].GetArray()) {
                if (!item.IsObject() || !item.HasMember("type") || !item["type"].IsString()) continue;
                std::string_view type(item["type"].GetString(), item["type"].GetStringLength());
                if (type != "function_call") continue;
                ExtractedToolCall call;
                call.id = member_string(item, "call_id");
                call.name = member_string(item, "name");
                call.arguments = member_string(item, "arguments");
                calls.push_back(std::move(call));
            }
        }
    }
    return calls;
}

std::string response_text(const rj::Value& root) {
    if (!root.IsObject()) return {};
    if (root.HasMember("choices") && root["choices"].IsArray() && !root["choices"].Empty()) {
        const auto& choice = root["choices"][0];
        if (choice.IsObject() && choice.HasMember("message") && choice["message"].IsObject()) {
            const auto& message = choice["message"];
            if (message.HasMember("content") && message["content"].IsString()) return message["content"].GetString();
        }
        if (choice.IsObject() && choice.HasMember("text") && choice["text"].IsString()) return choice["text"].GetString();
    }
    if (root.HasMember("output_text") && root["output_text"].IsString()) return root["output_text"].GetString();
    if (root.HasMember("content") && root["content"].IsArray()) {
        std::string text;
        for (const auto& block : root["content"].GetArray()) {
            if (block.IsObject() && block.HasMember("text") && block["text"].IsString()) text += block["text"].GetString();
        }
        if (!text.empty()) return text;
    }
    if (root.HasMember("output") && root["output"].IsArray()) {
        std::string text;
        for (const auto& item : root["output"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("content") || !item["content"].IsArray()) continue;
            for (const auto& block : item["content"].GetArray()) {
                if (block.IsObject() && block.HasMember("text") && block["text"].IsString()) text += block["text"].GetString();
            }
        }
        if (!text.empty()) return text;
    }
    if (root.HasMember("candidates") && root["candidates"].IsArray() && !root["candidates"].Empty()) {
        const auto& candidate = root["candidates"][0];
        if (candidate.IsObject() && candidate.HasMember("content") && candidate["content"].IsObject()) {
            const auto& content = candidate["content"];
            if (content.HasMember("parts") && content["parts"].IsArray()) {
                std::string text;
                for (const auto& part : content["parts"].GetArray())
                    if (part.IsObject() && part.HasMember("text") && part["text"].IsString()) text += part["text"].GetString();
                return text;
            }
        }
    }
    return {};
}

bool has_unsupported_response_shape(const rj::Value& root, Format from) {
    if (from == Format::OpenAIChatCompletions && root.HasMember("choices")
        && root["choices"].IsArray() && !root["choices"].Empty()) {
        const auto& choice = root["choices"][0];
        if (!choice.IsObject() || !choice.HasMember("message") || !choice["message"].IsObject())
            return false;
        const auto& message = choice["message"];
        // Tool calls are now supported; only reject refusals and non-string content
        return message.HasMember("refusal")
            || (message.HasMember("content") && !message["content"].IsString()
                && !message["content"].IsNull());
    }
    if (from == Format::Anthropic && root.HasMember("content") && root["content"].IsArray()) {
        for (const auto& block : root["content"].GetArray()) {
            if (!block.IsObject() || !block.HasMember("type") || !block["type"].IsString()) return true;
            std::string_view type(block["type"].GetString(), block["type"].GetStringLength());
            // Allow text and tool_use blocks; reject everything else
            if (type != "text" && type != "tool_use") return true;
        }
    }
    if (from == Format::OpenAIResponses && root.HasMember("output")
        && root["output"].IsArray()) {
        for (const auto& item : root["output"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("type") || !item["type"].IsString()) return true;
            std::string_view type(item["type"].GetString(), item["type"].GetStringLength());
            // Allow message and function_call items
            if (type != "message" && type != "function_call") return true;
            if (type == "message" && item.HasMember("content") && item["content"].IsArray()) {
                for (const auto& block : item["content"].GetArray()) {
                    if (!block.IsObject() || !block.HasMember("type") || !block["type"].IsString()) return true;
                    std::string_view btype(block["type"].GetString(), block["type"].GetStringLength());
                    if (btype != "output_text") return true;
                }
            }
        }
    }
    if (from == Format::Gemini && root.HasMember("candidates")
        && root["candidates"].IsArray()) {
        for (const auto& candidate : root["candidates"].GetArray()) {
            if (!candidate.IsObject() || !candidate.HasMember("content")
                || !candidate["content"].IsObject()) continue;
            const auto& content = candidate["content"];
            if (!content.HasMember("parts") || !content["parts"].IsArray()) continue;
            for (const auto& part : content["parts"].GetArray()) {
                if (!part.IsObject()) return true;
                // Allow text and functionCall parts
                if (!part.HasMember("text") && !part.HasMember("functionCall"))
                    return true;
            }
        }
    }
    return false;
}

std::string response_model(const rj::Value& root, std::string_view fallback) {
    auto model = member_string(root, "model");
    if (!model.empty()) return model;
    model = member_string(root, "id");
    return model.empty() ? std::string(fallback) : model;
}

int usage_integer(const rj::Value& usage,
                  std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (!usage.IsObject() || !usage.HasMember(key)) continue;
        const auto& value = usage[key];
        if (value.IsInt()) return std::max(0, value.GetInt());
        if (value.IsInt64()) return static_cast<int>(std::min<int64_t>(
            std::numeric_limits<int>::max(), std::max<int64_t>(0, value.GetInt64())));
        if (value.IsUint()) return static_cast<int>(std::min<unsigned>(
            std::numeric_limits<int>::max(), value.GetUint()));
    }
    return 0;
}

void add_converted_usage(const rj::Value& source, Format to,
                         rj::Document& output) {
    const rj::Value* usage = nullptr;
    if (source.HasMember("usage") && source["usage"].IsObject()) usage = &source["usage"];
    if (!usage && source.HasMember("usageMetadata") && source["usageMetadata"].IsObject())
        usage = &source["usageMetadata"];
    if (!usage) return;
    auto input = usage_integer(*usage, {"input_tokens", "prompt_tokens", "promptTokenCount"});
    auto output_tokens = usage_integer(*usage,
        {"output_tokens", "completion_tokens", "candidatesTokenCount"});
    auto cache_create = usage_integer(*usage, {"cache_creation_input_tokens"});
    auto cache_read = usage_integer(*usage,
        {"cache_read_input_tokens", "cachedContentTokenCount"});
    if (usage->HasMember("prompt_tokens_details") && (*usage)["prompt_tokens_details"].IsObject())
        cache_read = std::max(cache_read,
            usage_integer((*usage)["prompt_tokens_details"], {"cached_tokens"}));
    if (usage->HasMember("input_tokens_details") && (*usage)["input_tokens_details"].IsObject())
        cache_read = std::max(cache_read,
            usage_integer((*usage)["input_tokens_details"], {"cached_tokens"}));

    auto& alloc = output.GetAllocator();
    rj::Value converted(rj::kObjectType);
    if (to == Format::OpenAIChatCompletions) {
        converted.AddMember("prompt_tokens", input, alloc);
        converted.AddMember("completion_tokens", output_tokens, alloc);
        converted.AddMember("total_tokens", input + output_tokens, alloc);
        if (cache_read > 0) {
            rj::Value details(rj::kObjectType);
            details.AddMember("cached_tokens", cache_read, alloc);
            converted.AddMember("prompt_tokens_details", details, alloc);
        }
        output.AddMember("usage", converted, alloc);
    } else if (to == Format::OpenAIResponses) {
        converted.AddMember("input_tokens", input, alloc);
        converted.AddMember("output_tokens", output_tokens, alloc);
        converted.AddMember("total_tokens", input + output_tokens, alloc);
        if (cache_read > 0) {
            rj::Value details(rj::kObjectType);
            details.AddMember("cached_tokens", cache_read, alloc);
            converted.AddMember("input_tokens_details", details, alloc);
        }
        output.AddMember("usage", converted, alloc);
    } else if (to == Format::Anthropic) {
        converted.AddMember("input_tokens", input, alloc);
        converted.AddMember("output_tokens", output_tokens, alloc);
        if (cache_create > 0)
            converted.AddMember("cache_creation_input_tokens", cache_create, alloc);
        if (cache_read > 0)
            converted.AddMember("cache_read_input_tokens", cache_read, alloc);
        output.AddMember("usage", converted, alloc);
    } else if (to == Format::Gemini) {
        converted.AddMember("promptTokenCount", input, alloc);
        converted.AddMember("candidatesTokenCount", output_tokens, alloc);
        converted.AddMember("totalTokenCount", input + output_tokens, alloc);
        if (cache_read > 0)
            converted.AddMember("cachedContentTokenCount", cache_read, alloc);
        output.AddMember("usageMetadata", converted, alloc);
    }
}

std::string error_member(const rj::Value& root, const char* key) {
    return root.IsObject() && root.HasMember(key) && root[key].IsString()
        ? std::string(root[key].GetString(), root[key].GetStringLength())
        : std::string{};
}

struct CanonicalError {
    std::string type;
    std::string message;
    std::string code;
};

CanonicalError canonical_error(std::string_view body, int status_code) {
    CanonicalError result;
    rj::Document source;
    source.Parse(body.data(), body.size());
    const rj::Value* error = nullptr;
    if (!source.HasParseError() && source.IsObject()) {
        if (source.HasMember("error") && source["error"].IsObject())
            error = &source["error"];
        else
            error = &source;
    }
    if (error) {
        result.type = error_member(*error, "type");
        result.message = error_member(*error, "message");
        result.code = error_member(*error, "code");
        if (result.message.empty() && error->HasMember("detail")
            && (*error)["detail"].IsString())
            result.message = error_member(*error, "detail");
    }
    if (result.message.empty()) result.message = "Provider request failed";

    const auto type = result.type;
    // HTTP status is authoritative when it carries a standard protocol
    // meaning; provider payload labels are only a fallback for ambiguous 4xx
    // responses and non-standard gateways.
    if (status_code == 401)
        result.type = "authentication_error";
    else if (status_code == 403)
        result.type = "permission_error";
    else if (status_code == 404)
        result.type = "not_found_error";
    else if (status_code == 408 || status_code == 504)
        result.type = "timeout_error";
    else if (status_code == 429)
        result.type = "rate_limit_error";
    else if (status_code >= 500)
        result.type = "provider_error";
    else if (type.find("auth") != std::string::npos)
        result.type = "authentication_error";
    else if (type.find("rate") != std::string::npos
        || type.find("quota") != std::string::npos)
        result.type = "rate_limit_error";
    else if (type.find("permission") != std::string::npos
        || type.find("forbidden") != std::string::npos)
        result.type = "permission_error";
    else if (type.find("not_found") != std::string::npos
        || type == "NOT_FOUND")
        result.type = "not_found_error";
    else if (type.find("invalid") != std::string::npos
        || type == "INVALID_ARGUMENT")
        result.type = "invalid_request_error";
    else if (status_code >= 400 && status_code < 500)
        result.type = "invalid_request_error";
    else
        result.type = "provider_error";
    return result;
}

}  // namespace

std::string Converter::convert_response(std::string_view body, Format from, Format to,
                                        std::string_view requested_model) {
    return convert_response_checked(body, from, to, requested_model).body;
}

std::string Converter::convert_error(std::string_view body, int status_code,
                                     Format from, Format to) {
    if (from == to) return std::string(body);

    const auto canonical = canonical_error(body, status_code);
    rj::Document output;
    output.SetObject();
    auto& alloc = output.GetAllocator();
    const auto add_string = [&](rj::Value& object, const char* key,
                                std::string_view value) {
        object.AddMember(rj::Value(key, alloc),
            rj::Value(value.data(), static_cast<rj::SizeType>(value.size()), alloc), alloc);
    };

    if (to == Format::Anthropic) {
        rj::Value error(rj::kObjectType);
        add_string(error, "type", canonical.type);
        add_string(error, "message", canonical.message);
        output.AddMember("type", "error", alloc);
        output.AddMember("error", error, alloc);
    } else if (to == Format::Gemini) {
        rj::Value error(rj::kObjectType);
        error.AddMember("code", status_code, alloc);
        add_string(error, "message", canonical.message);
        std::string status = canonical.type == "invalid_request_error"
            ? "INVALID_ARGUMENT"
            : canonical.type == "authentication_error" ? "UNAUTHENTICATED"
            : canonical.type == "permission_error" ? "PERMISSION_DENIED"
            : canonical.type == "not_found_error" ? "NOT_FOUND"
            : canonical.type == "rate_limit_error" ? "RESOURCE_EXHAUSTED"
            : canonical.type == "timeout_error" ? "DEADLINE_EXCEEDED" : "INTERNAL";
        add_string(error, "status", status);
        output.AddMember("error", error, alloc);
    } else {
        rj::Value error(rj::kObjectType);
        add_string(error, "message", canonical.message);
        add_string(error, "type", canonical.type);
        error.AddMember("param", rj::Value().SetNull(), alloc);
        if (!canonical.code.empty()) add_string(error, "code", canonical.code);
        output.AddMember("error", error, alloc);
    }

    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    output.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetSize());
}

ResponseConversionResult Converter::convert_response_checked(
    std::string_view body, Format from, Format to,
    std::string_view requested_model) {
    if (from == to) return {true, std::string(body), {}};
    rj::Document source;
    source.Parse(body.data(), body.size());
    if (source.HasParseError() || !source.IsObject())
        return {false, {}, "Upstream response is not a JSON object"};
    if (has_unsupported_response_shape(source, from))
        return {false, {}, "Upstream response contains unsupported tool or multimodal content"};
    const auto text = response_text(source);
    const auto model = response_model(source, requested_model);
    const auto finish = parse_finish_reason(from, source);
    const auto tool_calls = extract_tool_calls(from, source);
    const auto effective_finish = tool_calls.empty() ? finish : FinishReason::ToolCalls;
    const auto finish_str = serialize_finish_reason(effective_finish, to);

    rj::Document output;
    output.SetObject();
    auto& alloc = output.GetAllocator();
    if (to == Format::OpenAIChatCompletions) {
        output.AddMember("id", rj::Value("chatcmpl-gateway", alloc), alloc);
        output.AddMember("object", rj::Value("chat.completion", alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        rj::Value choices(rj::kArrayType), choice(rj::kObjectType), message(rj::kObjectType);
        message.AddMember("role", rj::Value("assistant", alloc), alloc);
        if (text.empty() && !tool_calls.empty()) {
            message.AddMember("content", rj::Value().SetNull(), alloc);
        } else {
            message.AddMember("content", rj::Value(text.c_str(), alloc), alloc);
        }
        if (!tool_calls.empty()) {
            rj::Value tcs(rj::kArrayType);
            for (const auto& tc : tool_calls) {
                rj::Value tc_obj(rj::kObjectType);
                tc_obj.AddMember("id", rj::Value(tc.id.c_str(), alloc), alloc);
                tc_obj.AddMember("type", "function", alloc);
                rj::Value fn(rj::kObjectType);
                fn.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
                fn.AddMember("arguments", rj::Value(tc.arguments.c_str(), alloc), alloc);
                tc_obj.AddMember("function", fn, alloc);
                tcs.PushBack(tc_obj, alloc);
            }
            message.AddMember("tool_calls", tcs, alloc);
        }
        choice.AddMember("index", 0, alloc);
        choice.AddMember("message", message, alloc);
        choice.AddMember("finish_reason", rj::Value(finish_str.c_str(), alloc), alloc);
        choices.PushBack(choice, alloc);
        output.AddMember("choices", choices, alloc);
    } else if (to == Format::Anthropic) {
        output.AddMember("id", rj::Value("msg_gateway", alloc), alloc);
        output.AddMember("type", rj::Value("message", alloc), alloc);
        output.AddMember("role", rj::Value("assistant", alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        rj::Value content(rj::kArrayType);
        if (!text.empty()) {
            rj::Value block(rj::kObjectType);
            block.AddMember("type", rj::Value("text", alloc), alloc);
            block.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
            content.PushBack(block, alloc);
        }
        for (const auto& tc : tool_calls) {
            rj::Value block(rj::kObjectType);
            block.AddMember("type", rj::Value("tool_use", alloc), alloc);
            block.AddMember("id", rj::Value(tc.id.c_str(), alloc), alloc);
            block.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
            rj::Document input_doc;
            if (!tc.arguments.empty()) input_doc.Parse(tc.arguments.c_str());
            if (input_doc.HasParseError() || !input_doc.IsObject()) input_doc.SetObject();
            block.AddMember("input", rj::Value(input_doc, alloc), alloc);
            content.PushBack(block, alloc);
        }
        if (content.Empty()) {
            rj::Value block(rj::kObjectType);
            block.AddMember("type", rj::Value("text", alloc), alloc);
            block.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
            content.PushBack(block, alloc);
        }
        output.AddMember("content", content, alloc);
        output.AddMember("stop_reason", rj::Value(finish_str.c_str(), alloc), alloc);
    } else if (to == Format::OpenAIResponses) {
        output.AddMember("id", rj::Value("resp_gateway", alloc), alloc);
        output.AddMember("object", rj::Value("response", alloc), alloc);
        output.AddMember("status", rj::Value(finish_str.c_str(), alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        output.AddMember("output_text", rj::Value(text.c_str(), alloc), alloc);
        rj::Value output_items(rj::kArrayType);
        if (!text.empty() || tool_calls.empty()) {
            rj::Value item(rj::kObjectType), content_arr(rj::kArrayType), block(rj::kObjectType);
            item.AddMember("type", rj::Value("message", alloc), alloc);
            item.AddMember("role", rj::Value("assistant", alloc), alloc);
            block.AddMember("type", rj::Value("output_text", alloc), alloc);
            block.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
            content_arr.PushBack(block, alloc);
            item.AddMember("content", content_arr, alloc);
            output_items.PushBack(item, alloc);
        }
        for (const auto& tc : tool_calls) {
            rj::Value item(rj::kObjectType);
            item.AddMember("type", rj::Value("function_call", alloc), alloc);
            item.AddMember("call_id", rj::Value(tc.id.c_str(), alloc), alloc);
            item.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
            item.AddMember("arguments", rj::Value(tc.arguments.c_str(), alloc), alloc);
            output_items.PushBack(item, alloc);
        }
        output.AddMember("output", output_items, alloc);
    } else if (to == Format::Gemini) {
        rj::Value candidates(rj::kArrayType), candidate(rj::kObjectType), content(rj::kObjectType), parts(rj::kArrayType);
        if (!text.empty()) {
            rj::Value part(rj::kObjectType);
            part.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
            parts.PushBack(part, alloc);
        }
        for (const auto& tc : tool_calls) {
            rj::Value part(rj::kObjectType);
            rj::Value fc(rj::kObjectType);
            fc.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
            rj::Document args;
            if (!tc.arguments.empty()) args.Parse(tc.arguments.c_str());
            if (args.HasParseError() || !args.IsObject()) args.SetObject();
            fc.AddMember("args", rj::Value(args, alloc), alloc);
            part.AddMember("functionCall", fc, alloc);
            parts.PushBack(part, alloc);
        }
        if (parts.Empty()) {
            rj::Value part(rj::kObjectType);
            part.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
            parts.PushBack(part, alloc);
        }
        content.AddMember("role", rj::Value("model", alloc), alloc);
        content.AddMember("parts", parts, alloc);
        candidate.AddMember("content", content, alloc);
        candidate.AddMember("finishReason", rj::Value(finish_str.c_str(), alloc), alloc);
        candidates.PushBack(candidate, alloc);
        output.AddMember("candidates", candidates, alloc);
    } else {
        return {false, {}, "Unsupported response conversion target"};
    }

    add_converted_usage(source, to, output);
    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    output.Accept(writer);
    return {true, buffer.GetString(), {}};
}

}  // namespace gateway::protocol
