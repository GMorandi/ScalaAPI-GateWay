#include "server/websocket.h"

#include <array>
#include <cstring>

namespace gateway::server {

bool is_websocket_upgrade(std::string_view upgrade_hdr, std::string_view connection_hdr) {
    if (upgrade_hdr.empty()) return false;
    bool has_websocket = false;
    for (size_t i = 0; i + 9 <= upgrade_hdr.size(); ++i) {
        if (strncasecmp(upgrade_hdr.data() + i, "websocket", 9) == 0) {
            has_websocket = true;
            break;
        }
    }
    if (!has_websocket) return false;

    for (size_t i = 0; i + 7 <= connection_hdr.size(); ++i) {
        if (strncasecmp(connection_hdr.data() + i, "upgrade", 7) == 0)
            return true;
    }
    return false;
}

struct Sha1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

static uint32_t rol(uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void sha1_transform(uint32_t state[5], const uint8_t buf[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(buf[i*4]) << 24) | (uint32_t(buf[i*4+1]) << 16) |
               (uint32_t(buf[i*4+2]) << 8) | uint32_t(buf[i*4+3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }

        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    Sha1Ctx ctx;
    ctx.state[0] = 0x67452301; ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE; ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
    ctx.count = 0;

    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        sha1_transform(ctx.state, data + i);

    uint8_t final[128] = {};
    size_t rem = len - i;
    memcpy(final, data + i, rem);
    final[rem] = 0x80;
    size_t final_len = (rem < 56) ? 64 : 128;
    uint64_t bits = uint64_t(len) * 8;
    for (int j = 0; j < 8; ++j)
        final[final_len - 1 - j] = uint8_t(bits >> (j * 8));

    for (size_t off = 0; off < final_len; off += 64)
        sha1_transform(ctx.state, final + off);

    std::array<uint8_t, 20> digest;
    for (int j = 0; j < 5; ++j) {
        digest[j*4]   = uint8_t(ctx.state[j] >> 24);
        digest[j*4+1] = uint8_t(ctx.state[j] >> 16);
        digest[j*4+2] = uint8_t(ctx.state[j] >> 8);
        digest[j*4+3] = uint8_t(ctx.state[j]);
    }
    return digest;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i+1]) << 8;
        if (i + 2 < len) n |= uint32_t(data[i+2]);
        out += B64[(n >> 18) & 63];
        out += B64[(n >> 12) & 63];
        out += (i + 1 < len) ? B64[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? B64[n & 63] : '=';
    }
    return out;
}

std::string compute_websocket_accept(std::string_view key) {
    static constexpr const char* MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined(key);
    combined += MAGIC;
    auto digest = sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
    return base64_encode(digest.data(), digest.size());
}

bool parse_ws_frame(const uint8_t* data, size_t len, WsFrame& frame, size_t& consumed) {
    if (len < 2) return false;

    frame.fin = (data[0] & 0x80) != 0;
    frame.opcode = data[0] & 0x0F;
    bool masked = (data[1] & 0x80) != 0;
    uint64_t payload_len = data[1] & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (len < 4) return false;
        payload_len = (uint64_t(data[2]) << 8) | data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | data[2 + i];
        offset = 10;
    }

    uint8_t mask_key[4] = {};
    if (masked) {
        if (len < offset + 4) return false;
        memcpy(mask_key, data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) return false;

    frame.payload.assign(reinterpret_cast<const char*>(data + offset), payload_len);
    if (masked) {
        for (size_t i = 0; i < payload_len; ++i)
            frame.payload[i] ^= mask_key[i % 4];
    }

    consumed = offset + payload_len;
    return true;
}

std::string encode_ws_frame(uint8_t opcode, std::string_view payload, bool mask) {
    std::string out;
    out += char(0x80 | opcode);

    uint8_t mask_bit = mask ? 0x80 : 0x00;
    if (payload.size() < 126) {
        out += char(mask_bit | uint8_t(payload.size()));
    } else if (payload.size() < 65536) {
        out += char(mask_bit | 126);
        out += char(payload.size() >> 8);
        out += char(payload.size() & 0xFF);
    } else {
        out += char(mask_bit | 127);
        for (int i = 7; i >= 0; --i)
            out += char((payload.size() >> (i * 8)) & 0xFF);
    }

    out += payload;
    return out;
}

}  // namespace gateway::server
