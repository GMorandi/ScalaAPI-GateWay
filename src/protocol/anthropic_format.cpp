#include "protocol/chat_ir.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <string_view>

namespace gateway::protocol::anthropic {

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
    if (doc.HasMember("max_tokens") && doc["max_tokens"].IsInt())
        req.max_tokens = doc["max_tokens"].GetInt();
    if (doc.HasMember("temperature") && doc["temperature"].IsNumber())
        req.temperature = doc["temperature"].GetDouble();
    if (doc.HasMember("top_p") && doc["top_p"].IsNumber())
        req.top_p = doc["top_p"].GetDouble();

    if (doc.HasMember("system")) {
        auto& sys = doc["system"];
        if (sys.IsString()) {
            req.system = sys.GetString();
        } else if (sys.IsArray()) {
            for (auto& block : sys.GetArray()) {
                if (block.IsObject() && block.HasMember("text") && block["text"].IsString()) {
                    if (!req.system.empty()) req.system += "\n";
                    req.system += block["text"].GetString();
                }
            }
        }
    }

    if (doc.HasMember("stop_sequences") && doc["stop_sequences"].IsArray())
        for (auto& v : doc["stop_sequences"].GetArray())
            if (v.IsString()) req.stop.push_back(v.GetString());

    if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
        auto& meta = doc["metadata"];
        if (meta.HasMember("user_id") && meta["user_id"].IsString())
            req.metadata_user_id = meta["user_id"].GetString();
    }

    if (doc.HasMember("messages") && doc["messages"].IsArray()) {
        for (auto& m : doc["messages"].GetArray()) {
            if (!m.IsObject()) continue;
            Message msg;
            msg.role = get_str(m, "role");

            if (m.HasMember("content")) {
                auto& c = m["content"];
                if (c.IsString()) {
                    msg.add_text(c.GetString());
                } else if (c.IsArray()) {
                    for (auto& block : c.GetArray()) {
                        if (!block.IsObject()) continue;
                        auto type = get_str(block, "type");
                        if (type == "text") {
                            msg.add_text(get_str(block, "text"));
                        } else if (type == "tool_use") {
                            ContentBlock b;
                            b.type = ContentBlock::Type::ToolUse;
                            b.tool_call_id = get_str(block, "id");
                            b.tool_name = get_str(block, "name");
                            if (block.HasMember("input") && block["input"].IsObject()) {
                                rj::StringBuffer sb;
                                rj::Writer<rj::StringBuffer> w(sb);
                                block["input"].Accept(w);
                                b.tool_arguments = sb.GetString();
                            }
                            msg.content.push_back(std::move(b));

                            ToolCall tc;
                            tc.id = b.tool_call_id;
                            tc.name = b.tool_name;
                            tc.arguments = b.tool_arguments;
                            msg.tool_calls.push_back(std::move(tc));
                        } else if (type == "tool_result") {
                            ContentBlock b;
                            b.type = ContentBlock::Type::ToolResult;
                            b.tool_call_id = get_str(block, "tool_use_id");
                            if (block.HasMember("content")) {
                                if (block["content"].IsString())
                                    b.text = block["content"].GetString();
                                else if (block["content"].IsArray()) {
                                    for (auto& sub : block["content"].GetArray())
                                        if (sub.IsObject() && sub.HasMember("text"))
                                            b.text += sub["text"].GetString();
                                }
                            }
                            msg.content.push_back(std::move(b));
                            msg.tool_call_id = b.tool_call_id;
                        }
                    }
                }
            }
            req.messages.push_back(std::move(msg));
        }
    }

    if (doc.HasMember("tools") && doc["tools"].IsArray()) {
        for (auto& t : doc["tools"].GetArray()) {
            if (!t.IsObject()) continue;
            ToolDef def;
            def.name = get_str(t, "name");
            def.description = get_str(t, "description");
            if (t.HasMember("input_schema") && t["input_schema"].IsObject()) {
                rj::StringBuffer sb;
                rj::Writer<rj::StringBuffer> w(sb);
                t["input_schema"].Accept(w);
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
    doc.AddMember("max_tokens", req.max_tokens > 0 ? req.max_tokens : 4096, alloc);

    if (!req.system.empty())
        doc.AddMember("system", rj::Value(req.system.c_str(), alloc), alloc);
    if (req.stream) doc.AddMember("stream", true, alloc);
    if (req.temperature) doc.AddMember("temperature", *req.temperature, alloc);
    if (req.top_p) doc.AddMember("top_p", *req.top_p, alloc);
    if (!req.stop.empty()) {
        rj::Value arr(rj::kArrayType);
        for (auto& s : req.stop) arr.PushBack(rj::Value(s.c_str(), alloc), alloc);
        doc.AddMember("stop_sequences", arr, alloc);
    }
    if (!req.metadata_user_id.empty()) {
        rj::Value meta(rj::kObjectType);
        meta.AddMember("user_id", rj::Value(req.metadata_user_id.c_str(), alloc), alloc);
        doc.AddMember("metadata", meta, alloc);
    }

    rj::Value messages(rj::kArrayType);
    for (auto& msg : req.messages) {
        rj::Value m(rj::kObjectType);
        m.AddMember("role", rj::Value(msg.role.c_str(), alloc), alloc);

        bool needs_blocks = false;
        for (auto& b : msg.content)
            if (b.type != ContentBlock::Type::Text) needs_blocks = true;
        if (!msg.tool_calls.empty()) needs_blocks = true;

        if (!needs_blocks) {
            m.AddMember("content", rj::Value(msg.text_content().c_str(), alloc), alloc);
        } else {
            rj::Value content_arr(rj::kArrayType);
            for (auto& b : msg.content) {
                rj::Value block(rj::kObjectType);
                if (b.type == ContentBlock::Type::Text) {
                    block.AddMember("type", "text", alloc);
                    block.AddMember("text", rj::Value(b.text.c_str(), alloc), alloc);
                } else if (b.type == ContentBlock::Type::ToolUse) {
                    block.AddMember("type", "tool_use", alloc);
                    block.AddMember("id", rj::Value(b.tool_call_id.c_str(), alloc), alloc);
                    block.AddMember("name", rj::Value(b.tool_name.c_str(), alloc), alloc);
                    rj::Document input;
                    if (!b.tool_arguments.empty())
                        input.Parse(b.tool_arguments.c_str());
                    if (input.HasParseError() || !input.IsObject()) input.SetObject();
                    block.AddMember("input", rj::Value(input, alloc), alloc);
                } else if (b.type == ContentBlock::Type::ToolResult) {
                    block.AddMember("type", "tool_result", alloc);
                    block.AddMember("tool_use_id", rj::Value(b.tool_call_id.c_str(), alloc), alloc);
                    block.AddMember("content", rj::Value(b.text.c_str(), alloc), alloc);
                }
                content_arr.PushBack(block, alloc);
            }
            m.AddMember("content", content_arr, alloc);
        }
        messages.PushBack(m, alloc);
    }
    doc.AddMember("messages", messages, alloc);

    if (!req.tools.empty()) {
        rj::Value tools(rj::kArrayType);
        for (auto& t : req.tools) {
            rj::Value tool(rj::kObjectType);
            tool.AddMember("name", rj::Value(t.name.c_str(), alloc), alloc);
            if (!t.description.empty())
                tool.AddMember("description", rj::Value(t.description.c_str(), alloc), alloc);
            rj::Document schema;
            if (!t.parameters_json.empty())
                schema.Parse(t.parameters_json.c_str());
            if (schema.HasParseError() || !schema.IsObject()) schema.SetObject();
            tool.AddMember("input_schema", rj::Value(schema, alloc), alloc);
            tools.PushBack(tool, alloc);
        }
        doc.AddMember("tools", tools, alloc);
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
    case StreamDelta::Type::MessageStart: {
        event_type = "message_start";
        doc.AddMember("type", "message_start", alloc);
        rj::Value message(rj::kObjectType);
        message.AddMember("id", "msg_stream", alloc);
        message.AddMember("type", "message", alloc);
        message.AddMember("role", "assistant", alloc);
        message.AddMember("model", rj::Value(delta.model.c_str(), alloc), alloc);
        rj::Value usage(rj::kObjectType);
        usage.AddMember("input_tokens", delta.input_tokens, alloc);
        usage.AddMember("output_tokens", 0, alloc);
        message.AddMember("usage", usage, alloc);
        doc.AddMember("message", message, alloc);
        break;
    }
    case StreamDelta::Type::ContentStart: {
        event_type = "content_block_start";
        doc.AddMember("type", "content_block_start", alloc);
        doc.AddMember("index", delta.index, alloc);
        rj::Value block(rj::kObjectType);
        if (!delta.tool_name.empty()) {
            block.AddMember("type", "tool_use", alloc);
            block.AddMember("id", rj::Value(delta.tool_call_id.c_str(), alloc), alloc);
            block.AddMember("name", rj::Value(delta.tool_name.c_str(), alloc), alloc);
            rj::Value input(rj::kObjectType);
            block.AddMember("input", input, alloc);
        } else {
            block.AddMember("type", "text", alloc);
            block.AddMember("text", "", alloc);
        }
        doc.AddMember("content_block", block, alloc);
        break;
    }
    case StreamDelta::Type::TextDelta: {
        event_type = "content_block_delta";
        doc.AddMember("type", "content_block_delta", alloc);
        doc.AddMember("index", delta.index, alloc);
        rj::Value d(rj::kObjectType);
        d.AddMember("type", "text_delta", alloc);
        d.AddMember("text", rj::Value(delta.text.c_str(), alloc), alloc);
        doc.AddMember("delta", d, alloc);
        break;
    }
    case StreamDelta::Type::ToolCallDelta: {
        event_type = "content_block_delta";
        doc.AddMember("type", "content_block_delta", alloc);
        doc.AddMember("index", delta.index, alloc);
        rj::Value d(rj::kObjectType);
        d.AddMember("type", "input_json_delta", alloc);
        d.AddMember("partial_json", rj::Value(delta.tool_arguments_delta.c_str(), alloc), alloc);
        doc.AddMember("delta", d, alloc);
        break;
    }
    case StreamDelta::Type::ContentEnd: {
        event_type = "content_block_stop";
        doc.AddMember("type", "content_block_stop", alloc);
        doc.AddMember("index", delta.index, alloc);
        break;
    }
    case StreamDelta::Type::MessageEnd: {
        event_type = "message_delta";
        doc.AddMember("type", "message_delta", alloc);
        rj::Value d(rj::kObjectType);
        std::string reason = delta.finish_reason.empty() ? "end_turn" : delta.finish_reason;
        if (reason == "stop") reason = "end_turn";
        else if (reason == "tool_calls") reason = "tool_use";
        d.AddMember("stop_reason", rj::Value(reason.c_str(), alloc), alloc);
        doc.AddMember("delta", d, alloc);
        rj::Value usage(rj::kObjectType);
        usage.AddMember("output_tokens", delta.output_tokens, alloc);
        doc.AddMember("usage", usage, alloc);
        break;
    }
    case StreamDelta::Type::Done: {
        event_type = "message_stop";
        doc.AddMember("type", "message_stop", alloc);
        break;
    }
    }

    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> w(sb);
    doc.Accept(w);
    return "event: " + event_type + "\ndata: " + sb.GetString() + "\n\n";
}

StreamDelta parse_stream_event(std::string_view event_type, std::string_view data) {
    StreamDelta delta;
    rj::Document doc;
    doc.Parse(data.data(), data.size());
    if (doc.HasParseError() || !doc.IsObject()) return delta;

    if (event_type == "message_start") {
        delta.type = StreamDelta::Type::MessageStart;
        if (doc.HasMember("message") && doc["message"].IsObject()) {
            auto& msg = doc["message"];
            if (msg.HasMember("model") && msg["model"].IsString())
                delta.model = msg["model"].GetString();
            if (msg.HasMember("usage") && msg["usage"].IsObject()) {
                auto& u = msg["usage"];
                if (u.HasMember("input_tokens") && u["input_tokens"].IsInt())
                    delta.input_tokens = u["input_tokens"].GetInt();
            }
        }
    } else if (event_type == "content_block_start") {
        delta.type = StreamDelta::Type::ContentStart;
        if (doc.HasMember("index") && doc["index"].IsInt())
            delta.index = doc["index"].GetInt();
        if (doc.HasMember("content_block") && doc["content_block"].IsObject()) {
            auto& block = doc["content_block"];
            if (get_str(block, "type") == "tool_use") {
                delta.tool_call_id = get_str(block, "id");
                delta.tool_name = get_str(block, "name");
            }
        }
    } else if (event_type == "content_block_delta") {
        if (doc.HasMember("index") && doc["index"].IsInt())
            delta.index = doc["index"].GetInt();
        if (doc.HasMember("delta") && doc["delta"].IsObject()) {
            auto& d = doc["delta"];
            auto dtype = get_str(d, "type");
            if (dtype == "text_delta") {
                delta.type = StreamDelta::Type::TextDelta;
                delta.text = get_str(d, "text");
            } else if (dtype == "input_json_delta") {
                delta.type = StreamDelta::Type::ToolCallDelta;
                delta.tool_arguments_delta = get_str(d, "partial_json");
            }
        }
    } else if (event_type == "content_block_stop") {
        delta.type = StreamDelta::Type::ContentEnd;
        if (doc.HasMember("index") && doc["index"].IsInt())
            delta.index = doc["index"].GetInt();
    } else if (event_type == "message_delta") {
        delta.type = StreamDelta::Type::MessageEnd;
        if (doc.HasMember("delta") && doc["delta"].IsObject()) {
            auto reason = get_str(doc["delta"], "stop_reason");
            if (reason == "end_turn") delta.finish_reason = "stop";
            else if (reason == "tool_use") delta.finish_reason = "tool_calls";
            else delta.finish_reason = reason;
        }
        if (doc.HasMember("usage") && doc["usage"].IsObject()) {
            auto& u = doc["usage"];
            if (u.HasMember("output_tokens") && u["output_tokens"].IsInt())
                delta.output_tokens = u["output_tokens"].GetInt();
        }
    } else if (event_type == "message_stop") {
        delta.type = StreamDelta::Type::Done;
    }

    return delta;
}

}  // namespace gateway::protocol::anthropic
