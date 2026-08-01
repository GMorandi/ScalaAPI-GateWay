#include "protocol/chat_ir.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <string_view>

namespace gateway::protocol::openai {

namespace rj = rapidjson;

static std::string get_str(const rj::Value& obj, const char* key) {
    if (obj.HasMember(key) && obj[key].IsString())
        return obj[key].GetString();
    return {};
}

static void extract_content_blocks(const rj::Value& content_val, Message& msg) {
    if (content_val.IsString()) {
        msg.add_text(content_val.GetString());
    } else if (content_val.IsArray()) {
        for (auto& part : content_val.GetArray()) {
            if (!part.IsObject()) continue;
            auto type = get_str(part, "type");
            if (type == "text") {
                msg.add_text(get_str(part, "text"));
            } else if (type == "tool_use") {
                ContentBlock b;
                b.type = ContentBlock::Type::ToolUse;
                b.tool_call_id = get_str(part, "id");
                b.tool_name = get_str(part, "name");
                if (part.HasMember("input") && part["input"].IsObject()) {
                    rj::StringBuffer sb;
                    rj::Writer<rj::StringBuffer> w(sb);
                    part["input"].Accept(w);
                    b.tool_arguments = sb.GetString();
                }
                msg.content.push_back(std::move(b));
            } else if (type == "tool_result") {
                ContentBlock b;
                b.type = ContentBlock::Type::ToolResult;
                b.tool_call_id = get_str(part, "tool_use_id");
                if (part.HasMember("content") && part["content"].IsString())
                    b.text = part["content"].GetString();
                msg.content.push_back(std::move(b));
            }
        }
    }
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
    else if (doc.HasMember("max_completion_tokens") && doc["max_completion_tokens"].IsInt())
        req.max_tokens = doc["max_completion_tokens"].GetInt();
    if (doc.HasMember("temperature") && doc["temperature"].IsNumber())
        req.temperature = doc["temperature"].GetDouble();
    if (doc.HasMember("top_p") && doc["top_p"].IsNumber())
        req.top_p = doc["top_p"].GetDouble();
    if (doc.HasMember("user") && doc["user"].IsString())
        req.metadata_user_id = doc["user"].GetString();

    if (doc.HasMember("stop")) {
        auto& s = doc["stop"];
        if (s.IsString()) req.stop.push_back(s.GetString());
        else if (s.IsArray())
            for (auto& v : s.GetArray())
                if (v.IsString()) req.stop.push_back(v.GetString());
    }

    if (doc.HasMember("messages") && doc["messages"].IsArray()) {
        for (auto& m : doc["messages"].GetArray()) {
            if (!m.IsObject()) continue;
            Message msg;
            msg.role = get_str(m, "role");

            if (msg.role == "system" || msg.role == "developer") {
                if (m.HasMember("content")) {
                    std::string sys_text;
                    if (m["content"].IsString()) sys_text = m["content"].GetString();
                    else if (m["content"].IsArray()) {
                        for (auto& p : m["content"].GetArray())
                            if (p.IsObject() && p.HasMember("text") && p["text"].IsString())
                                sys_text += p["text"].GetString();
                    }
                    if (!req.system.empty()) req.system += "\n";
                    req.system += sys_text;
                }
                continue;
            }

            if (m.HasMember("content") && !m["content"].IsNull())
                extract_content_blocks(m["content"], msg);

            if (m.HasMember("tool_calls") && m["tool_calls"].IsArray()) {
                for (auto& tc : m["tool_calls"].GetArray()) {
                    if (!tc.IsObject()) continue;
                    ToolCall call;
                    call.id = get_str(tc, "id");
                    if (tc.HasMember("function") && tc["function"].IsObject()) {
                        call.name = get_str(tc["function"], "name");
                        call.arguments = get_str(tc["function"], "arguments");
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
            }

            if (m.HasMember("tool_call_id") && m["tool_call_id"].IsString())
                msg.tool_call_id = m["tool_call_id"].GetString();

            req.messages.push_back(std::move(msg));
        }
    }

    if (doc.HasMember("tools") && doc["tools"].IsArray()) {
        for (auto& t : doc["tools"].GetArray()) {
            if (!t.IsObject()) continue;
            if (get_str(t, "type") != "function") continue;
            ToolDef def;
            if (t.HasMember("function") && t["function"].IsObject()) {
                auto& fn = t["function"];
                def.name = get_str(fn, "name");
                def.description = get_str(fn, "description");
                if (fn.HasMember("parameters") && fn["parameters"].IsObject()) {
                    rj::StringBuffer sb;
                    rj::Writer<rj::StringBuffer> w(sb);
                    fn["parameters"].Accept(w);
                    def.parameters_json = sb.GetString();
                }
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

    rj::Value messages(rj::kArrayType);
    if (!req.system.empty()) {
        rj::Value sys(rj::kObjectType);
        sys.AddMember("role", "system", alloc);
        sys.AddMember("content", rj::Value(req.system.c_str(), alloc), alloc);
        messages.PushBack(sys, alloc);
    }

    for (auto& msg : req.messages) {
        rj::Value m(rj::kObjectType);
        m.AddMember("role", rj::Value(msg.role.c_str(), alloc), alloc);

        if (!msg.tool_call_id.empty())
            m.AddMember("tool_call_id", rj::Value(msg.tool_call_id.c_str(), alloc), alloc);

        bool has_tool_blocks = false;
        for (auto& b : msg.content)
            if (b.type == ContentBlock::Type::ToolUse || b.type == ContentBlock::Type::ToolResult)
                has_tool_blocks = true;

        if (!has_tool_blocks && msg.tool_calls.empty()) {
            m.AddMember("content", rj::Value(msg.text_content().c_str(), alloc), alloc);
        } else {
            rj::Value content_arr(rj::kArrayType);
            for (auto& b : msg.content) {
                rj::Value block(rj::kObjectType);
                if (b.type == ContentBlock::Type::Text) {
                    block.AddMember("type", "text", alloc);
                    block.AddMember("text", rj::Value(b.text.c_str(), alloc), alloc);
                } else if (b.type == ContentBlock::Type::ToolResult) {
                    block.AddMember("type", "tool_result", alloc);
                    block.AddMember("tool_use_id", rj::Value(b.tool_call_id.c_str(), alloc), alloc);
                    block.AddMember("content", rj::Value(b.text.c_str(), alloc), alloc);
                }
                content_arr.PushBack(block, alloc);
            }
            m.AddMember("content", content_arr, alloc);
        }

        if (!msg.tool_calls.empty()) {
            rj::Value tcs(rj::kArrayType);
            for (auto& tc : msg.tool_calls) {
                rj::Value tc_obj(rj::kObjectType);
                tc_obj.AddMember("id", rj::Value(tc.id.c_str(), alloc), alloc);
                tc_obj.AddMember("type", "function", alloc);
                rj::Value fn(rj::kObjectType);
                fn.AddMember("name", rj::Value(tc.name.c_str(), alloc), alloc);
                fn.AddMember("arguments", rj::Value(tc.arguments.c_str(), alloc), alloc);
                tc_obj.AddMember("function", fn, alloc);
                tcs.PushBack(tc_obj, alloc);
            }
            m.AddMember("tool_calls", tcs, alloc);
        }

        messages.PushBack(m, alloc);
    }
    doc.AddMember("messages", messages, alloc);

    if (req.stream) doc.AddMember("stream", true, alloc);
    if (req.max_tokens > 0) doc.AddMember("max_tokens", req.max_tokens, alloc);
    if (req.temperature) doc.AddMember("temperature", *req.temperature, alloc);
    if (req.top_p) doc.AddMember("top_p", *req.top_p, alloc);
    if (!req.stop.empty()) {
        rj::Value arr(rj::kArrayType);
        for (auto& s : req.stop) arr.PushBack(rj::Value(s.c_str(), alloc), alloc);
        doc.AddMember("stop", arr, alloc);
    }
    if (!req.metadata_user_id.empty())
        doc.AddMember("user", rj::Value(req.metadata_user_id.c_str(), alloc), alloc);

    if (!req.tools.empty()) {
        rj::Value tools(rj::kArrayType);
        for (auto& t : req.tools) {
            rj::Value tool(rj::kObjectType);
            tool.AddMember("type", "function", alloc);
            rj::Value fn(rj::kObjectType);
            fn.AddMember("name", rj::Value(t.name.c_str(), alloc), alloc);
            if (!t.description.empty())
                fn.AddMember("description", rj::Value(t.description.c_str(), alloc), alloc);
            if (!t.parameters_json.empty()) {
                rj::Document params;
                params.Parse(t.parameters_json.c_str());
                if (!params.HasParseError())
                    fn.AddMember("parameters", rj::Value(params, alloc), alloc);
            }
            tool.AddMember("function", fn, alloc);
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
    if (delta.type == StreamDelta::Type::Done)
        return "data: [DONE]\n\n";

    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("object", "chat.completion.chunk", alloc);
    if (!delta.model.empty())
        doc.AddMember("model", rj::Value(delta.model.c_str(), alloc), alloc);

    rj::Value choices(rj::kArrayType);
    rj::Value choice(rj::kObjectType);
    choice.AddMember("index", delta.index, alloc);

    rj::Value d(rj::kObjectType);
    switch (delta.type) {
    case StreamDelta::Type::MessageStart:
        d.AddMember("role", "assistant", alloc);
        d.AddMember("content", "", alloc);
        break;
    case StreamDelta::Type::TextDelta:
        d.AddMember("content", rj::Value(delta.text.c_str(), alloc), alloc);
        break;
    case StreamDelta::Type::ToolCallDelta: {
        rj::Value tcs(rj::kArrayType);
        rj::Value tc(rj::kObjectType);
        tc.AddMember("index", delta.index, alloc);
        if (!delta.tool_call_id.empty())
            tc.AddMember("id", rj::Value(delta.tool_call_id.c_str(), alloc), alloc);
        if (!delta.tool_name.empty()) {
            tc.AddMember("type", "function", alloc);
            rj::Value fn(rj::kObjectType);
            fn.AddMember("name", rj::Value(delta.tool_name.c_str(), alloc), alloc);
            fn.AddMember("arguments", "", alloc);
            tc.AddMember("function", fn, alloc);
        } else if (!delta.tool_arguments_delta.empty()) {
            rj::Value fn(rj::kObjectType);
            fn.AddMember("name", "", alloc);
            fn.AddMember("arguments", rj::Value(delta.tool_arguments_delta.c_str(), alloc), alloc);
            tc.AddMember("function", fn, alloc);
        }
        tcs.PushBack(tc, alloc);
        d.AddMember("tool_calls", tcs, alloc);
        break;
    }
    case StreamDelta::Type::MessageEnd:
        break;
    default:
        break;
    }

    choice.AddMember("delta", d, alloc);
    if (!delta.finish_reason.empty())
        choice.AddMember("finish_reason", rj::Value(delta.finish_reason.c_str(), alloc), alloc);
    else
        choice.AddMember("finish_reason", rj::Value(), alloc);
    choices.PushBack(choice, alloc);
    doc.AddMember("choices", choices, alloc);

    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> w(sb);
    doc.Accept(w);
    return "data: " + std::string(sb.GetString()) + "\n\n";
}

StreamDelta parse_stream_event(std::string_view data) {
    StreamDelta delta;
    if (data == "[DONE]") {
        delta.type = StreamDelta::Type::Done;
        return delta;
    }

    rj::Document doc;
    doc.Parse(data.data(), data.size());
    if (doc.HasParseError() || !doc.IsObject()) return delta;

    if (doc.HasMember("model") && doc["model"].IsString())
        delta.model = doc["model"].GetString();

    if (!doc.HasMember("choices") || !doc["choices"].IsArray() ||
        doc["choices"].GetArray().Empty())
        return delta;

    auto& choice = doc["choices"][0];
    if (choice.HasMember("finish_reason") && choice["finish_reason"].IsString())
        delta.finish_reason = choice["finish_reason"].GetString();

    if (!choice.HasMember("delta") || !choice["delta"].IsObject())
        return delta;
    auto& d = choice["delta"];

    if (d.HasMember("role") && d.HasMember("content")) {
        delta.type = StreamDelta::Type::MessageStart;
        return delta;
    }

    if (d.HasMember("content") && d["content"].IsString()) {
        delta.type = StreamDelta::Type::TextDelta;
        delta.text = d["content"].GetString();
        return delta;
    }

    if (d.HasMember("tool_calls") && d["tool_calls"].IsArray() &&
        !d["tool_calls"].GetArray().Empty()) {
        auto& tc = d["tool_calls"][0];
        delta.type = StreamDelta::Type::ToolCallDelta;
        if (tc.HasMember("index") && tc["index"].IsInt())
            delta.index = tc["index"].GetInt();
        if (tc.HasMember("id") && tc["id"].IsString())
            delta.tool_call_id = tc["id"].GetString();
        if (tc.HasMember("function") && tc["function"].IsObject()) {
            auto& fn = tc["function"];
            if (fn.HasMember("name") && fn["name"].IsString())
                delta.tool_name = fn["name"].GetString();
            if (fn.HasMember("arguments") && fn["arguments"].IsString())
                delta.tool_arguments_delta = fn["arguments"].GetString();
        }
        return delta;
    }

    if (!delta.finish_reason.empty()) {
        delta.type = StreamDelta::Type::MessageEnd;
    }
    return delta;
}

}  // namespace gateway::protocol::openai
