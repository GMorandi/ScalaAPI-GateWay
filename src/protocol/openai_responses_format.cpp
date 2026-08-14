#include "protocol/chat_ir.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <string_view>

namespace gateway::protocol::openai_responses {

namespace rj = rapidjson;

static std::string get_str(const rj::Value& obj, const char* key) {
    if (obj.HasMember(key) && obj[key].IsString())
        return obj[key].GetString();
    return {};
}

ChatRequest parse_request(std::string_view body) {
    ChatRequest req;
    rj::Document doc;
    doc.Parse(body.data(), body.size());
    if (doc.HasParseError() || !doc.IsObject()) return req;

    req.model = get_str(doc, "model");
    if (doc.HasMember("stream") && doc["stream"].IsBool())
        req.stream = doc["stream"].GetBool();
    if (doc.HasMember("max_output_tokens") && doc["max_output_tokens"].IsInt())
        req.max_tokens = doc["max_output_tokens"].GetInt();
    else if (doc.HasMember("max_tokens") && doc["max_tokens"].IsInt())
        req.max_tokens = doc["max_tokens"].GetInt();
    if (doc.HasMember("temperature") && doc["temperature"].IsNumber())
        req.temperature = doc["temperature"].GetDouble();
    if (doc.HasMember("top_p") && doc["top_p"].IsNumber())
        req.top_p = doc["top_p"].GetDouble();
    if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
        auto& meta = doc["metadata"];
        if (meta.HasMember("user_id") && meta["user_id"].IsString())
            req.metadata_user_id = meta["user_id"].GetString();
    }

    if (doc.HasMember("instructions") && doc["instructions"].IsString())
        req.system = doc["instructions"].GetString();

    if (doc.HasMember("input")) {
        auto& input = doc["input"];
        if (input.IsString()) {
            Message msg;
            msg.role = "user";
            msg.add_text(input.GetString());
            req.messages.push_back(std::move(msg));
        } else if (input.IsArray()) {
            for (auto& item : input.GetArray()) {
                if (!item.IsObject()) continue;
                auto type = get_str(item, "type");

                if (type == "message" || item.HasMember("role")) {
                    Message msg;
                    msg.role = get_str(item, "role");
                    if (msg.role.empty()) msg.role = "user";

                    if (msg.role == "system" || msg.role == "developer") {
                        if (item.HasMember("content") && item["content"].IsString()) {
                            if (!req.system.empty()) req.system += "\n";
                            req.system += item["content"].GetString();
                        }
                        continue;
                    }

                    if (item.HasMember("content")) {
                        auto& content = item["content"];
                        if (content.IsString()) {
                            msg.add_text(content.GetString());
                        } else if (content.IsArray()) {
                            for (auto& part : content.GetArray()) {
                                if (!part.IsObject()) continue;
                                auto pt = get_str(part, "type");
                                if (pt == "input_text" || pt == "text" || pt == "output_text") {
                                    msg.add_text(get_str(part, "text"));
                                } else {
                                    req.unsupported_content = true;
                                }
                            }
                        }
                    }
                    req.messages.push_back(std::move(msg));
                } else if (type == "function_call") {
                    Message msg;
                    msg.role = "assistant";
                    ToolCall tc;
                    tc.id = get_str(item, "call_id");
                    tc.name = get_str(item, "name");
                    tc.arguments = get_str(item, "arguments");
                    msg.tool_calls.push_back(std::move(tc));
                    req.messages.push_back(std::move(msg));
                } else if (type == "function_call_output") {
                    Message msg;
                    msg.role = "tool";
                    msg.tool_call_id = get_str(item, "call_id");
                    msg.add_text(get_str(item, "output"));
                    req.messages.push_back(std::move(msg));
                }
            }
        }
    }

    if (doc.HasMember("tools") && doc["tools"].IsArray()) {
        for (auto& t : doc["tools"].GetArray()) {
            if (!t.IsObject()) continue;
            if (get_str(t, "type") != "function") continue;
            ToolDef def;
            def.name = get_str(t, "name");
            def.description = get_str(t, "description");
            if (t.HasMember("parameters") && t["parameters"].IsObject()) {
                rj::StringBuffer sb;
                rj::Writer<rj::StringBuffer> w(sb);
                t["parameters"].Accept(w);
                def.parameters_json = sb.GetString();
            }
            req.tools.push_back(std::move(def));
        }
    }

    return req;
}

std::string serialize_request(const ChatRequest& req) {
    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("model", rj::Value(req.model.c_str(), alloc), alloc);

    if (!req.system.empty())
        doc.AddMember("instructions", rj::Value(req.system.c_str(), alloc), alloc);

    rj::Value input(rj::kArrayType);
    for (auto& msg : req.messages) {
        rj::Value item(rj::kObjectType);
        item.AddMember("type", "message", alloc);
        item.AddMember("role", rj::Value(msg.role.c_str(), alloc), alloc);

        if (!msg.tool_calls.empty()) {
            for (auto& tc : msg.tool_calls) {
                rj::Value fc(rj::kObjectType);
                fc.AddMember("type", "function_call", alloc);
                fc.AddMember("call_id", rj::Value(tc.id.c_str(), alloc), alloc);
                fc.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
                fc.AddMember("arguments", rj::Value(tc.arguments.c_str(), alloc), alloc);
                input.PushBack(fc, alloc);
            }
            continue;
        }

        if (msg.role == "tool" && !msg.tool_call_id.empty()) {
            item.SetObject();
            item.AddMember("type", "function_call_output", alloc);
            item.AddMember("call_id", rj::Value(msg.tool_call_id.c_str(), alloc), alloc);
            item.AddMember("output", rj::Value(msg.text_content().c_str(), alloc), alloc);
            input.PushBack(item, alloc);
            continue;
        }

        rj::Value content(rj::kArrayType);
        rj::Value part(rj::kObjectType);
        part.AddMember("type", "input_text", alloc);
        part.AddMember("text", rj::Value(msg.text_content().c_str(), alloc), alloc);
        content.PushBack(part, alloc);
        item.AddMember("content", content, alloc);
        input.PushBack(item, alloc);
    }
    doc.AddMember("input", input, alloc);

    if (req.stream) doc.AddMember("stream", true, alloc);
    if (req.max_tokens > 0) doc.AddMember("max_output_tokens", req.max_tokens, alloc);
    if (req.temperature) doc.AddMember("temperature", *req.temperature, alloc);
    if (req.top_p) doc.AddMember("top_p", *req.top_p, alloc);

    if (!req.tools.empty()) {
        rj::Value tools(rj::kArrayType);
        for (auto& t : req.tools) {
            rj::Value tool(rj::kObjectType);
            tool.AddMember("type", "function", alloc);
            tool.AddMember("name", rj::Value(t.name.c_str(), alloc), alloc);
            if (!t.description.empty())
                tool.AddMember("description", rj::Value(t.description.c_str(), alloc), alloc);
            if (!t.parameters_json.empty()) {
                rj::Document params;
                params.Parse(t.parameters_json.c_str());
                if (!params.HasParseError())
                    tool.AddMember("parameters", rj::Value(params, alloc), alloc);
            }
            tools.PushBack(tool, alloc);
        }
        doc.AddMember("tools", tools, alloc);
    }

    if (!req.metadata_user_id.empty()) {
        rj::Value meta(rj::kObjectType);
        meta.AddMember("user_id", rj::Value(req.metadata_user_id.c_str(), alloc), alloc);
        doc.AddMember("metadata", meta, alloc);
    }

    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> w(sb);
    doc.Accept(w);
    return sb.GetString();
}

std::string serialize_stream_event(const StreamDelta& delta) {
    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    std::string event_type;

    switch (delta.type) {
    case StreamDelta::Type::MessageStart:
        event_type = "response.created";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::ContentStart:
        event_type = "response.output_item.added";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::TextDelta:
        event_type = "response.output_text.delta";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        doc.AddMember("delta", rj::Value(delta.text.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::ToolCallDelta:
        event_type = "response.function_call_arguments.delta";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        doc.AddMember("delta", rj::Value(delta.tool_arguments_delta.c_str(), alloc), alloc);
        if (!delta.tool_call_id.empty())
            doc.AddMember("call_id", rj::Value(delta.tool_call_id.c_str(), alloc), alloc);
        if (!delta.tool_name.empty())
            doc.AddMember("name", rj::Value(delta.tool_name.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::MessageEnd:
        event_type = "response.completed";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::Done:
        return "event: response.completed\ndata: {\"type\":\"response.completed\"}\n\n";
    default:
        event_type = "response.output_text.delta";
        doc.AddMember("type", rj::Value(event_type.c_str(), alloc), alloc);
        break;
    }

    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> w(sb);
    doc.Accept(w);
    return "event: " + event_type + "\ndata: " + sb.GetString() + "\n\n";
}

StreamDelta parse_stream_event(std::string_view data) {
    StreamDelta delta;

    rj::Document doc;
    doc.Parse(data.data(), data.size());
    if (doc.HasParseError() || !doc.IsObject()) return delta;

    auto type = get_str(doc, "type");

    if (type == "response.created") {
        delta.type = StreamDelta::Type::MessageStart;
    } else if (type == "response.output_item.added") {
        delta.type = StreamDelta::Type::ContentStart;
    } else if (type == "response.output_text.delta") {
        delta.type = StreamDelta::Type::TextDelta;
        delta.text = get_str(doc, "delta");
    } else if (type == "response.function_call_arguments.delta") {
        delta.type = StreamDelta::Type::ToolCallDelta;
        delta.tool_arguments_delta = get_str(doc, "delta");
        delta.tool_call_id = get_str(doc, "call_id");
        delta.tool_name = get_str(doc, "name");
    } else if (type == "response.completed") {
        delta.type = StreamDelta::Type::Done;
    } else if (type == "response.output_text.done" ||
               type == "response.output_item.done" ||
               type == "response.content_part.done" ||
               type == "response.content_part.added") {
        delta.type = StreamDelta::Type::ContentEnd;
    }

    return delta;
}

}  // namespace gateway::protocol::openai_responses
