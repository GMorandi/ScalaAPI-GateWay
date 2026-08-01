#include "protocol/chat_ir.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <string_view>

namespace gateway::protocol::gemini {

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

    if (doc.HasMember("model") && doc["model"].IsString())
        req.model = doc["model"].GetString();

    if (doc.HasMember("systemInstruction") && doc["systemInstruction"].IsObject()) {
        auto& si = doc["systemInstruction"];
        if (si.HasMember("parts") && si["parts"].IsArray()) {
            for (auto& p : si["parts"].GetArray()) {
                if (p.IsObject() && p.HasMember("text") && p["text"].IsString()) {
                    if (!req.system.empty()) req.system += "\n";
                    req.system += p["text"].GetString();
                }
            }
        }
    }

    if (doc.HasMember("generationConfig") && doc["generationConfig"].IsObject()) {
        auto& gc = doc["generationConfig"];
        if (gc.HasMember("maxOutputTokens") && gc["maxOutputTokens"].IsInt())
            req.max_tokens = gc["maxOutputTokens"].GetInt();
        if (gc.HasMember("temperature") && gc["temperature"].IsNumber())
            req.temperature = gc["temperature"].GetDouble();
        if (gc.HasMember("topP") && gc["topP"].IsNumber())
            req.top_p = gc["topP"].GetDouble();
        if (gc.HasMember("stopSequences") && gc["stopSequences"].IsArray())
            for (auto& v : gc["stopSequences"].GetArray())
                if (v.IsString()) req.stop.push_back(v.GetString());
    }

    if (doc.HasMember("contents") && doc["contents"].IsArray()) {
        for (auto& c : doc["contents"].GetArray()) {
            if (!c.IsObject()) continue;
            Message msg;
            auto role = get_str(c, "role");
            msg.role = (role == "model") ? "assistant" : role;

            if (c.HasMember("parts") && c["parts"].IsArray()) {
                for (auto& p : c["parts"].GetArray()) {
                    if (!p.IsObject()) continue;
                    if (p.HasMember("text") && p["text"].IsString()) {
                        msg.add_text(p["text"].GetString());
                    } else if (p.HasMember("functionCall") && p["functionCall"].IsObject()) {
                        auto& fc = p["functionCall"];
                        ContentBlock b;
                        b.type = ContentBlock::Type::ToolUse;
                        b.tool_name = get_str(fc, "name");
                        b.tool_call_id = b.tool_name;
                        if (fc.HasMember("args") && fc["args"].IsObject()) {
                            rj::StringBuffer sb;
                            rj::Writer<rj::StringBuffer> w(sb);
                            fc["args"].Accept(w);
                            b.tool_arguments = sb.GetString();
                        }
                        ToolCall tc;
                        tc.name = b.tool_name;
                        tc.id = b.tool_call_id;
                        tc.arguments = b.tool_arguments;
                        msg.content.push_back(std::move(b));
                        msg.tool_calls.push_back(std::move(tc));
                    } else if (p.HasMember("functionResponse") && p["functionResponse"].IsObject()) {
                        auto& fr = p["functionResponse"];
                        ContentBlock b;
                        b.type = ContentBlock::Type::ToolResult;
                        b.tool_name = get_str(fr, "name");
                        b.tool_call_id = b.tool_name;
                        if (fr.HasMember("response") && fr["response"].IsObject()) {
                            rj::StringBuffer sb;
                            rj::Writer<rj::StringBuffer> w(sb);
                            fr["response"].Accept(w);
                            b.text = sb.GetString();
                        }
                        msg.tool_call_id = b.tool_call_id;
                        msg.content.push_back(std::move(b));
                    }
                }
            }
            req.messages.push_back(std::move(msg));
        }
    }

    if (doc.HasMember("tools") && doc["tools"].IsArray()) {
        for (auto& t : doc["tools"].GetArray()) {
            if (!t.IsObject() || !t.HasMember("functionDeclarations")) continue;
            auto& fds = t["functionDeclarations"];
            if (!fds.IsArray()) continue;
            for (auto& fd : fds.GetArray()) {
                if (!fd.IsObject()) continue;
                ToolDef def;
                def.name = get_str(fd, "name");
                def.description = get_str(fd, "description");
                if (fd.HasMember("parameters") && fd["parameters"].IsObject()) {
                    rj::StringBuffer sb;
                    rj::Writer<rj::StringBuffer> w(sb);
                    fd["parameters"].Accept(w);
                    def.parameters_json = sb.GetString();
                }
                req.tools.push_back(std::move(def));
            }
        }
    }

    return req;
}

std::string serialize_request(const ChatRequest& req) {
    rj::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    if (!req.system.empty()) {
        rj::Value si(rj::kObjectType);
        rj::Value parts(rj::kArrayType);
        rj::Value part(rj::kObjectType);
        part.AddMember("text", rj::Value(req.system.c_str(), alloc), alloc);
        parts.PushBack(part, alloc);
        si.AddMember("parts", parts, alloc);
        doc.AddMember("systemInstruction", si, alloc);
    }

    rj::Value contents(rj::kArrayType);
    for (auto& msg : req.messages) {
        rj::Value c(rj::kObjectType);
        std::string role = (msg.role == "assistant") ? "model" : msg.role;
        c.AddMember("role", rj::Value(role.c_str(), alloc), alloc);

        rj::Value parts(rj::kArrayType);
        for (auto& b : msg.content) {
            rj::Value part(rj::kObjectType);
            if (b.type == ContentBlock::Type::Text) {
                part.AddMember("text", rj::Value(b.text.c_str(), alloc), alloc);
            } else if (b.type == ContentBlock::Type::ToolUse) {
                rj::Value fc(rj::kObjectType);
                fc.AddMember("name", rj::Value(b.tool_name.c_str(), alloc), alloc);
                rj::Document args;
                if (!b.tool_arguments.empty()) args.Parse(b.tool_arguments.c_str());
                if (args.HasParseError() || !args.IsObject()) args.SetObject();
                fc.AddMember("args", rj::Value(args, alloc), alloc);
                part.AddMember("functionCall", fc, alloc);
            } else if (b.type == ContentBlock::Type::ToolResult) {
                rj::Value fr(rj::kObjectType);
                fr.AddMember("name", rj::Value(b.tool_name.c_str(), alloc), alloc);
                rj::Document resp;
                resp.Parse(b.text.c_str());
                if (resp.HasParseError() || !resp.IsObject()) {
                    resp.SetObject();
                    resp.AddMember("result", rj::Value(b.text.c_str(), alloc), alloc);
                }
                fr.AddMember("response", rj::Value(resp, alloc), alloc);
                part.AddMember("functionResponse", fr, alloc);
            }
            parts.PushBack(part, alloc);
        }
        c.AddMember("parts", parts, alloc);
        contents.PushBack(c, alloc);
    }
    doc.AddMember("contents", contents, alloc);

    rj::Value gc(rj::kObjectType);
    if (req.max_tokens > 0) gc.AddMember("maxOutputTokens", req.max_tokens, alloc);
    if (req.temperature) gc.AddMember("temperature", *req.temperature, alloc);
    if (req.top_p) gc.AddMember("topP", *req.top_p, alloc);
    if (!req.stop.empty()) {
        rj::Value arr(rj::kArrayType);
        for (auto& s : req.stop) arr.PushBack(rj::Value(s.c_str(), alloc), alloc);
        gc.AddMember("stopSequences", arr, alloc);
    }
    if (gc.MemberCount() > 0) doc.AddMember("generationConfig", gc, alloc);

    if (!req.tools.empty()) {
        rj::Value tools(rj::kArrayType);
        rj::Value tool(rj::kObjectType);
        rj::Value fds(rj::kArrayType);
        for (auto& t : req.tools) {
            rj::Value fd(rj::kObjectType);
            fd.AddMember("name", rj::Value(t.name.c_str(), alloc), alloc);
            if (!t.description.empty())
                fd.AddMember("description", rj::Value(t.description.c_str(), alloc), alloc);
            if (!t.parameters_json.empty()) {
                rj::Document params;
                params.Parse(t.parameters_json.c_str());
                if (!params.HasParseError() && params.IsObject())
                    fd.AddMember("parameters", rj::Value(params, alloc), alloc);
            }
            fds.PushBack(fd, alloc);
        }
        tool.AddMember("functionDeclarations", fds, alloc);
        tools.PushBack(tool, alloc);
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

    rj::Value candidates(rj::kArrayType);
    rj::Value candidate(rj::kObjectType);

    rj::Value content(rj::kObjectType);
    content.AddMember("role", "model", alloc);
    rj::Value parts(rj::kArrayType);

    switch (delta.type) {
    case StreamDelta::Type::TextDelta: {
        rj::Value part(rj::kObjectType);
        part.AddMember("text", rj::Value(delta.text.c_str(), alloc), alloc);
        parts.PushBack(part, alloc);
        break;
    }
    case StreamDelta::Type::ToolCallDelta: {
        rj::Value part(rj::kObjectType);
        rj::Value fc(rj::kObjectType);
        fc.AddMember("name", rj::Value(delta.tool_name.c_str(), alloc), alloc);
        rj::Value args(rj::kObjectType);
        fc.AddMember("args", args, alloc);
        part.AddMember("functionCall", fc, alloc);
        parts.PushBack(part, alloc);
        break;
    }
    case StreamDelta::Type::MessageEnd: {
        rj::Value part(rj::kObjectType);
        part.AddMember("text", "", alloc);
        parts.PushBack(part, alloc);
        std::string reason = (delta.finish_reason == "tool_calls") ? "STOP" : "STOP";
        candidate.AddMember("finishReason", rj::Value(reason.c_str(), alloc), alloc);
        break;
    }
    default: {
        rj::Value part(rj::kObjectType);
        part.AddMember("text", "", alloc);
        parts.PushBack(part, alloc);
        break;
    }
    }

    content.AddMember("parts", parts, alloc);
    candidate.AddMember("content", content, alloc);
    candidate.AddMember("index", 0, alloc);
    candidates.PushBack(candidate, alloc);
    doc.AddMember("candidates", candidates, alloc);

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

    if (!doc.HasMember("candidates") || !doc["candidates"].IsArray() ||
        doc["candidates"].GetArray().Empty())
        return delta;

    auto& cand = doc["candidates"][0];
    if (cand.HasMember("finishReason") && cand["finishReason"].IsString()) {
        delta.finish_reason = "stop";
        delta.type = StreamDelta::Type::MessageEnd;
    }

    if (!cand.HasMember("content") || !cand["content"].IsObject())
        return delta;
    auto& content = cand["content"];
    if (!content.HasMember("parts") || !content["parts"].IsArray())
        return delta;

    for (auto& p : content["parts"].GetArray()) {
        if (!p.IsObject()) continue;
        if (p.HasMember("text") && p["text"].IsString()) {
            auto text = p["text"].GetString();
            if (text[0] != '\0') {
                delta.type = StreamDelta::Type::TextDelta;
                delta.text = text;
                return delta;
            }
        } else if (p.HasMember("functionCall") && p["functionCall"].IsObject()) {
            auto& fc = p["functionCall"];
            delta.type = StreamDelta::Type::ToolCallDelta;
            delta.tool_name = get_str(fc, "name");
            if (fc.HasMember("args") && fc["args"].IsObject()) {
                rj::StringBuffer sb;
                rj::Writer<rj::StringBuffer> w(sb);
                fc["args"].Accept(w);
                delta.tool_arguments_delta = sb.GetString();
            }
            return delta;
        }
    }

    return delta;
}

}  // namespace gateway::protocol::gemini
