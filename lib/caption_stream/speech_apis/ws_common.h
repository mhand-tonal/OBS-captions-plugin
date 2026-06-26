/******************************************************************************
Copyright (C) 2019 by <rat.with.a.compiler@gmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

// Shared RFC 6455 WebSocket helpers used by the curl-based speech backends
// (deepgram_websocket, local_websocket). These are transport primitives with
// no dependency on any backend's members, so they live here as free functions.

#ifndef OBS_CAPTION_STREAM_WS_COMMON_H
#define OBS_CAPTION_STREAM_WS_COMMON_H

#include <string>
#include <cstdint>
#include <cstddef>
#include <curl/curl.h>

#include "utils.h" // random_string

#ifndef _WIN32
#include <poll.h>
#endif


// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

static inline int ws_poll_socket(curl_socket_t fd, short events, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    return WSAPoll(&pfd, 1, timeout_ms);
#else
    struct pollfd pfd;
    pfd.fd = (int) fd;
    pfd.events = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
#endif
}


// ---------------------------------------------------------------------------
// Base64 encoder (for the WebSocket key)
// ---------------------------------------------------------------------------

static inline std::string ws_base64_encode(const unsigned char *data, size_t len) {
    static const char b64_table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = (unsigned int)(data[i]) << 16;
        if (i + 1 < len) n |= (unsigned int)(data[i + 1]) << 8;
        if (i + 2 < len) n |= (unsigned int)(data[i + 2]);

        out.push_back(b64_table[(n >> 18) & 0x3F]);
        out.push_back(b64_table[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64_table[n & 0x3F] : '=');
    }
    return out;
}

static inline std::string ws_generate_key() {
    std::string s = random_string(16);
    return ws_base64_encode((const unsigned char *) s.c_str(), 16);
}


// ---------------------------------------------------------------------------
// WebSocket frame parser
// ---------------------------------------------------------------------------

struct WsFrame {
    bool fin;
    uint8_t opcode;
    std::string payload;
};

// Parses one frame off the front of `buffer`, erasing the consumed bytes.
// Returns false (leaving buffer untouched) when a full frame isn't available yet.
static inline bool try_parse_ws_frame(std::string &buffer, WsFrame &frame) {
    if (buffer.size() < 2) return false;

    uint8_t b0 = (uint8_t) buffer[0];
    uint8_t b1 = (uint8_t) buffer[1];

    frame.fin = (b0 & 0x80) != 0;
    frame.opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t payload_len = b1 & 0x7F;

    size_t header_size = 2;
    if (payload_len == 126) {
        if (buffer.size() < 4) return false;
        payload_len = ((uint64_t)(uint8_t) buffer[2] << 8) | (uint8_t) buffer[3];
        header_size = 4;
    } else if (payload_len == 127) {
        if (buffer.size() < 10) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | (uint8_t) buffer[2 + i];
        header_size = 10;
    }

    if (masked) header_size += 4;

    if (buffer.size() < header_size + payload_len) return false;

    if (masked) {
        uint8_t mask[4];
        size_t mask_offset = header_size - 4;
        for (int i = 0; i < 4; i++)
            mask[i] = (uint8_t) buffer[mask_offset + i];
        frame.payload.resize(payload_len);
        for (size_t i = 0; i < payload_len; i++)
            frame.payload[i] = buffer[header_size + i] ^ mask[i % 4];
    } else {
        frame.payload = buffer.substr(header_size, payload_len);
    }

    buffer.erase(0, header_size + payload_len);
    return true;
}

#endif //OBS_CAPTION_STREAM_WS_COMMON_H
