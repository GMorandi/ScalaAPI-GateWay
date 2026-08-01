#pragma once

#include "protocol/chat_ir.h"
#include <string>
#include <string_view>

namespace gateway::protocol::openai {
ChatRequest parse_request(std::string_view body);
std::string serialize_request(const ChatRequest& req);
std::string serialize_stream_event(const StreamDelta& delta);
StreamDelta parse_stream_event(std::string_view data);
}

namespace gateway::protocol::anthropic {
ChatRequest parse_request(std::string_view body);
std::string serialize_request(const ChatRequest& req);
std::string serialize_stream_event(const StreamDelta& delta);
StreamDelta parse_stream_event(std::string_view event_type, std::string_view data);
}

namespace gateway::protocol::gemini {
ChatRequest parse_request(std::string_view body);
std::string serialize_request(const ChatRequest& req);
std::string serialize_stream_event(const StreamDelta& delta);
StreamDelta parse_stream_event(std::string_view data);
}
