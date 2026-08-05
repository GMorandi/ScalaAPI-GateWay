#include "protocol/converter.h"
#include "protocol/formats.h"
#include "platform/logging.h"

#include <simdjson.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <charconv>

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
    if (document.HasMember("dimensions")
        && (!document["dimensions"].IsInt() || document["dimensions"].GetInt() <= 0))
        return {false, "dimensions must be a positive integer"};
    if (document.HasMember("user") && !document["user"].IsString())
        return {false, "user must be a string"};
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
        return message.HasMember("tool_calls") || message.HasMember("function_call")
            || message.HasMember("refusal")
            || (message.HasMember("content") && !message["content"].IsString()
                && !message["content"].IsNull());
    }
    if (from == Format::Anthropic && root.HasMember("content") && root["content"].IsArray()) {
        for (const auto& block : root["content"].GetArray()) {
            if (!block.IsObject() || !block.HasMember("type") || !block["type"].IsString()
                || std::string_view(block["type"].GetString()) != "text") return true;
        }
    }
    if (from == Format::OpenAIResponses && root.HasMember("output")
        && root["output"].IsArray()) {
        for (const auto& item : root["output"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("type") || !item["type"].IsString()
                || std::string_view(item["type"].GetString()) != "message") return true;
            if (item.HasMember("content") && item["content"].IsArray()) {
                for (const auto& block : item["content"].GetArray()) {
                    if (!block.IsObject() || !block.HasMember("type") || !block["type"].IsString()
                        || std::string_view(block["type"].GetString()) != "output_text") return true;
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
                if (!part.IsObject() || !part.HasMember("text") || !part["text"].IsString())
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

}  // namespace

std::string Converter::convert_response(std::string_view body, Format from, Format to,
                                        std::string_view requested_model) {
    return convert_response_checked(body, from, to, requested_model).body;
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

    rj::Document output;
    output.SetObject();
    auto& alloc = output.GetAllocator();
    if (to == Format::OpenAIChatCompletions) {
        output.AddMember("id", rj::Value("chatcmpl-gateway", alloc), alloc);
        output.AddMember("object", rj::Value("chat.completion", alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        rj::Value choices(rj::kArrayType), choice(rj::kObjectType), message(rj::kObjectType);
        message.AddMember("role", rj::Value("assistant", alloc), alloc);
        message.AddMember("content", rj::Value(text.c_str(), alloc), alloc);
        choice.AddMember("index", 0, alloc);
        choice.AddMember("message", message, alloc);
        choice.AddMember("finish_reason", rj::Value("stop", alloc), alloc);
        choices.PushBack(choice, alloc);
        output.AddMember("choices", choices, alloc);
    } else if (to == Format::Anthropic) {
        output.AddMember("id", rj::Value("msg_gateway", alloc), alloc);
        output.AddMember("type", rj::Value("message", alloc), alloc);
        output.AddMember("role", rj::Value("assistant", alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        rj::Value content(rj::kArrayType), block(rj::kObjectType);
        block.AddMember("type", rj::Value("text", alloc), alloc);
        block.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
        content.PushBack(block, alloc);
        output.AddMember("content", content, alloc);
        output.AddMember("stop_reason", rj::Value("end_turn", alloc), alloc);
    } else if (to == Format::OpenAIResponses) {
        output.AddMember("id", rj::Value("resp_gateway", alloc), alloc);
        output.AddMember("object", rj::Value("response", alloc), alloc);
        output.AddMember("status", rj::Value("completed", alloc), alloc);
        output.AddMember("model", rj::Value(model.c_str(), alloc), alloc);
        output.AddMember("output_text", rj::Value(text.c_str(), alloc), alloc);
        rj::Value output_items(rj::kArrayType), item(rj::kObjectType), content(rj::kArrayType), block(rj::kObjectType);
        item.AddMember("type", rj::Value("message", alloc), alloc);
        item.AddMember("role", rj::Value("assistant", alloc), alloc);
        block.AddMember("type", rj::Value("output_text", alloc), alloc);
        block.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
        content.PushBack(block, alloc);
        item.AddMember("content", content, alloc);
        output_items.PushBack(item, alloc);
        output.AddMember("output", output_items, alloc);
    } else if (to == Format::Gemini) {
        rj::Value candidates(rj::kArrayType), candidate(rj::kObjectType), content(rj::kObjectType), parts(rj::kArrayType), part(rj::kObjectType);
        part.AddMember("text", rj::Value(text.c_str(), alloc), alloc);
        parts.PushBack(part, alloc);
        content.AddMember("role", rj::Value("model", alloc), alloc);
        content.AddMember("parts", parts, alloc);
        candidate.AddMember("content", content, alloc);
        candidate.AddMember("finishReason", rj::Value("STOP", alloc), alloc);
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
