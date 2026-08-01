#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace gateway::server {

bool is_websocket_upgrade(std::string_view upgrade_hdr, std::string_view connection_hdr);

std::string compute_websocket_accept(std::string_view key);

struct WsFrame {
    bool fin = true;
    uint8_t opcode = 0;
    std::string payload;
};

bool parse_ws_frame(const uint8_t* data, size_t len, WsFrame& frame, size_t& consumed);

std::string encode_ws_frame(uint8_t opcode, std::string_view payload, bool mask = false);

}  // namespace gateway::server
