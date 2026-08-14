#pragma once

#include <string>
#include <vector>
#include <optional>

namespace gateway::protocol {

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

enum class FinishReason {
    Stop,
    Length,
    ToolCalls,
    ContentFilter,
    Safety,
    Recitation,
    Unknown,
};

struct ContentBlock {
    enum class Type { Text, Image, ToolUse, ToolResult };
    Type type = Type::Text;
    std::string text;
    std::string tool_call_id;
    std::string tool_name;
    std::string tool_arguments;
};

struct Message {
    std::string role;
    std::vector<ContentBlock> content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;

    void add_text(const std::string& t) {
        ContentBlock b;
        b.type = ContentBlock::Type::Text;
        b.text = t;
        content.push_back(std::move(b));
    }

    std::string text_content() const {
        std::string out;
        for (auto& b : content) {
            if (b.type == ContentBlock::Type::Text) {
                if (!out.empty()) out += "\n";
                out += b.text;
            }
        }
        return out;
    }
};

struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters_json;
};

struct ChatRequest {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    std::vector<ToolDef> tools;
    bool stream = false;
    int max_tokens = 4096;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::vector<std::string> stop;
    std::string metadata_user_id;
    bool unsupported_content = false;
};

struct StreamDelta {
    enum class Type {
        MessageStart,
        ContentStart,
        TextDelta,
        ContentEnd,
        MessageEnd,
        ToolCallDelta,
        Done,
    };
    Type type = Type::TextDelta;
    std::string id;
    std::string text;
    std::string tool_call_id;
    std::string tool_name;
    std::string tool_arguments_delta;
    int index = 0;
    std::string model;
    std::string finish_reason;
    int input_tokens = 0;
    int output_tokens = 0;
};

}  // namespace gateway::protocol
