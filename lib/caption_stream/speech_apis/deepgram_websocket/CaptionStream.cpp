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

#include <utility>
#include <string>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <ctime>

#include "CaptionStream.h"
#include "utils.h"
#include "log.h"

#include <poll.h>
#include <fcntl.h>

#include <json11/json11.hpp>
using namespace json11;


// ---------------------------------------------------------------------------
// Base64 encoder (for WebSocket key)
// ---------------------------------------------------------------------------

static const char b64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static string base64_encode(const unsigned char *data, size_t len) {
    string out;
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

static string generate_ws_key() {
    string s = random_string(16);
    return base64_encode((const unsigned char *) s.c_str(), 16);
}


// ---------------------------------------------------------------------------
// WebSocket frame parser
// ---------------------------------------------------------------------------

struct WsFrame {
    bool fin;
    uint8_t opcode;
    string payload;
};

static bool try_parse_ws_frame(string &buffer, WsFrame &frame) {
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


// ---------------------------------------------------------------------------
// CaptionStream – lifecycle
// ---------------------------------------------------------------------------

static std::once_flag curl_init_flag;

DeepgramCaptionStream::DeepgramCaptionStream(CaptionStreamSettings settings)
        : settings(settings) {
    std::call_once(curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        srand((unsigned) time(nullptr));
    });
    debug_log("CaptionStream Deepgram WebSocket created");
}

bool DeepgramCaptionStream::start(std::shared_ptr<CaptionStream> self) {
    if (self.get() != this)
        return false;

    if (started)
        return false;

    started = true;
    stream_thread = new thread(&DeepgramCaptionStream::stream_run, this, self);
    return true;
}

void DeepgramCaptionStream::stream_run(std::shared_ptr<CaptionStream> self) {
    debug_log("starting Deepgram stream_run()");
    _stream_run();
    stop();
    debug_log("finished Deepgram stream_run()");
}


// ---------------------------------------------------------------------------
// CaptionStream – TLS connection via curl CONNECT_ONLY
// ---------------------------------------------------------------------------

bool DeepgramCaptionStream::curl_connect() {
    curl_handle = curl_easy_init();
    if (!curl_handle) {
        error_log("curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://api.deepgram.com");
    curl_easy_setopt(curl_handle, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, (long) settings.connect_timeout_ms);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    // Force HTTP/1.1 ALPN — without this, curl may negotiate HTTP/2 during TLS,
    // and the server will reject our raw HTTP/1.1 WebSocket upgrade request.
    curl_easy_setopt(curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        error_log("Deepgram connect failed: %s", curl_easy_strerror(res));
        return false;
    }

    res = curl_easy_getinfo(curl_handle, CURLINFO_ACTIVESOCKET, &sockfd);
    if (res != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        error_log("failed to get socket from curl");
        return false;
    }

    // non-blocking for the event loop
    int flags = fcntl((int) sockfd, F_GETFL, 0);
    if (flags >= 0)
        fcntl((int) sockfd, F_SETFL, flags | O_NONBLOCK);

    debug_log("Deepgram TLS connected");
    return true;
}


// ---------------------------------------------------------------------------
// CaptionStream – raw send / WebSocket framing
// ---------------------------------------------------------------------------

bool DeepgramCaptionStream::send_all(const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        CURLcode rc = curl_easy_send(curl_handle, data + sent, len - sent, &n);
        if (rc == CURLE_OK) {
            sent += n;
        } else if (rc == CURLE_AGAIN) {
            struct pollfd pfd;
            pfd.fd = (int) sockfd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, (int) settings.send_timeout_ms) <= 0)
                return false;
        } else {
            error_log("send error: %s", curl_easy_strerror(rc));
            return false;
        }
    }
    return true;
}

bool DeepgramCaptionStream::ws_send_frame(uint8_t opcode, const char *data, size_t len) {
    std::vector<uint8_t> frame;

    // FIN + opcode
    frame.push_back(0x80 | opcode);

    // MASK bit + payload length
    if (len < 126) {
        frame.push_back(0x80 | (uint8_t) len);
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
    }

    // 4-byte mask key
    uint8_t mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (uint8_t)(rand() & 0xFF);
    frame.insert(frame.end(), mask, mask + 4);

    // masked payload
    for (size_t i = 0; i < len; i++)
        frame.push_back(((uint8_t) data[i]) ^ mask[i % 4]);

    return send_all((const char *) frame.data(), frame.size());
}

bool DeepgramCaptionStream::ws_send_binary(const char *data, size_t len) {
    return ws_send_frame(0x02, data, len);
}

bool DeepgramCaptionStream::ws_send_text(const string &text) {
    return ws_send_frame(0x01, text.c_str(), text.size());
}


// ---------------------------------------------------------------------------
// CaptionStream – WebSocket handshake
// ---------------------------------------------------------------------------

bool DeepgramCaptionStream::ws_handshake() {
    string ws_key = generate_ws_key();

    // Build upgrade path with Deepgram query params
    string path = "/v1/listen?"
                  "encoding=linear16&sample_rate=16000&channels=1"
                  "&interim_results=true&punctuate=true&model=nova-2";

    if (!settings.language.empty()) {
        path += "&language=";
        path += settings.language;
    }

    if (settings.profanity_filter)
        path += "&profanity_filter=true";

    string request = "GET " + path + " HTTP/1.1\r\n"
                     "Host: api.deepgram.com\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: " + ws_key + "\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Authorization: Token " + settings.api_key + "\r\n"
                     "\r\n";

    if (!send_all(request.c_str(), request.size())) {
        error_log("failed to send WebSocket upgrade request");
        return false;
    }

    // Read upgrade response
    string response;
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(settings.recv_timeout_ms);

    while (response.find("\r\n\r\n") == string::npos) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) {
            error_log("WebSocket handshake timeout");
            return false;
        }

        struct pollfd pfd;
        pfd.fd = (int) sockfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, (int) std::min(remaining_ms, (long long) 1000));

        char buf[4096];
        size_t nread = 0;
        CURLcode rc = curl_easy_recv(curl_handle, buf, sizeof(buf), &nread);
        if (rc == CURLE_OK && nread > 0) {
            response.append(buf, nread);
        } else if (rc != CURLE_AGAIN) {
            error_log("handshake recv error: %s", curl_easy_strerror(rc));
            return false;
        }
    }

    // Verify 101 Switching Protocols
    if (response.find("HTTP/1.1 101") == string::npos) {
        if (response.find("401") != string::npos || response.find("403") != string::npos)
            error_log("Deepgram authentication failed — check your API key");

        size_t eol = response.find("\r\n");
        string status = (eol != string::npos) ? response.substr(0, eol) : response;
        error_log("WebSocket upgrade failed: %s", status.c_str());
        return false;
    }

    // Anything after the headers is already WebSocket data
    size_t header_end = response.find("\r\n\r\n") + 4;
    if (header_end < response.size())
        recv_buffer = response.substr(header_end);

    debug_log("Deepgram WebSocket connected, language=%s", settings.language.c_str());
    return true;
}


// ---------------------------------------------------------------------------
// CaptionStream – process incoming WebSocket frames
// ---------------------------------------------------------------------------

void DeepgramCaptionStream::process_incoming() {
    WsFrame frame;
    while (try_parse_ws_frame(recv_buffer, frame)) {
        switch (frame.opcode) {
            case 0x01: { // text frame — JSON from Deepgram
                string err;
                Json json = Json::parse(frame.payload, err);
                if (!err.empty()) {
                    info_log("Deepgram JSON parse error: %s", err.c_str());
                    break;
                }

                string msg_type = json["type"].string_value();
                if (msg_type != "Results") {
                    debug_log("Deepgram message: %s", msg_type.c_str());
                    break;
                }

                Json channel = json["channel"];
                Json alternatives = channel["alternatives"];
                if (!alternatives.is_array() || alternatives.array_items().empty())
                    break;

                string transcript = alternatives.array_items()[0]["transcript"].string_value();
                if (transcript.empty())
                    break;

                bool is_final = json["is_final"].bool_value();
                bool speech_final = json["speech_final"].bool_value();
                double confidence = alternatives.array_items()[0]["confidence"].number_value();

                auto now = std::chrono::steady_clock::now();
                if (update_first_received_at) {
                    first_received_at = now;
                    update_first_received_at = false;
                }

                double stability = is_final ? 1.0 : confidence;

                CaptionResult result(current_result_index, is_final, stability,
                                     transcript, frame.payload, first_received_at, now);
                result.speech_final = speech_final;

                // Parse word-level timestamps from final results
                if (is_final) {
                    Json words_json = alternatives.array_items()[0]["words"];
                    if (words_json.is_array()) {
                        for (auto &wj : words_json.array_items()) {
                            string w = wj["word"].string_value();
                            double ws = wj["start"].number_value();
                            double we = wj["end"].number_value();
                            if (!w.empty())
                                result.words.emplace_back(std::move(w), ws, we);
                        }
                    }
                }

                {
                    std::lock_guard<recursive_mutex> lock(on_caption_cb_handle.mutex);
                    if (on_caption_cb_handle.callback_fn)
                        on_caption_cb_handle.callback_fn(result);
                }

                if (is_final) {
                    update_first_received_at = true;
                    if (speech_final)
                        current_result_index++;
                }
                break;
            }
            case 0x09: // ping → pong
                ws_send_frame(0x0A, frame.payload.c_str(), frame.payload.size());
                break;
            case 0x0A: // pong — ignore
                break;
            case 0x08: // close
                info_log("Deepgram sent close frame");
                ws_send_frame(0x08, nullptr, 0);
                stop();
                return;
            default:
                debug_log("Deepgram unknown WS opcode: 0x%02x", frame.opcode);
                break;
        }
    }
}


// ---------------------------------------------------------------------------
// CaptionStream – main event loop
// ---------------------------------------------------------------------------

void DeepgramCaptionStream::_stream_run() {
    if (!curl_connect())
        return;

    if (is_stopped()) return;

    if (!ws_handshake())
        return;

    if (is_stopped()) return;

    debug_log("Deepgram stream active, entering event loop");

    while (!is_stopped()) {
        // 1. send all queued audio
        string *chunk;
        while (audio_queue.try_dequeue(chunk)) {
            if (!chunk->empty()) {
                if (!ws_send_binary(chunk->data(), chunk->size())) {
                    delete chunk;
                    error_log("Deepgram: failed to send audio chunk");
                    return;
                }
            }
            delete chunk;
            if (is_stopped()) return;
        }

        // 2. read all available incoming data
        bool got_data = false;
        while (true) {
            char buf[4096];
            size_t nread = 0;
            CURLcode rc = curl_easy_recv(curl_handle, buf, sizeof(buf), &nread);
            if (rc == CURLE_OK && nread > 0) {
                recv_buffer.append(buf, nread);
                got_data = true;
            } else if (rc == CURLE_AGAIN) {
                break; // no more data right now
            } else {
                if (rc == CURLE_OK)
                    info_log("Deepgram connection closed");
                else
                    error_log("Deepgram recv error: %s", curl_easy_strerror(rc));
                return;
            }
        }

        // 3. process any complete WebSocket frames
        if (got_data)
            process_incoming();

        // 4. brief wait for more data or audio
        struct pollfd pfd;
        pfd.fd = (int) sockfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, 20); // 20 ms
    }

    // graceful Deepgram close
    ws_send_text("{\"type\":\"CloseStream\"}");
    ws_send_frame(0x08, nullptr, 0);
}


// ---------------------------------------------------------------------------
// CaptionStream – audio queue
// ---------------------------------------------------------------------------

bool DeepgramCaptionStream::queue_audio_data(const char *audio_data, const uint data_size) {
    if (is_stopped())
        return false;

    string *str = new string(audio_data, data_size);

    if (settings.max_queue_depth) {
        int cleared_cnt = 0;
        while (audio_queue.size_approx() > settings.max_queue_depth) {
            string *item;
            if (audio_queue.try_dequeue(item)) {
                delete item;
                cleared_cnt++;
            }
        }
        if (cleared_cnt)
            debug_log("Deepgram queue overflow, dropped %d items", cleared_cnt);
    }

    audio_queue.enqueue(str);
    return true;
}

string *DeepgramCaptionStream::dequeue_audio_data(const std::int64_t timeout_us) {
    string *ret;
    if (audio_queue.wait_dequeue_timed(ret, timeout_us))
        return ret;
    return nullptr;
}


// ---------------------------------------------------------------------------
// CaptionStream – stop / cleanup
// ---------------------------------------------------------------------------

bool DeepgramCaptionStream::is_stopped() {
    return stopped;
}

bool DeepgramCaptionStream::is_started() {
    return started;
}

bool DeepgramCaptionStream::is_connected() {
    return started && !stopped && curl_handle != nullptr;
}

void DeepgramCaptionStream::stop() {
    info_log("Deepgram stop");
    on_caption_cb_handle.clear();
    stopped = true;

    // enqueue sentinel to unblock any dequeue waits
    string *to_unblock = new string();
    audio_queue.enqueue(to_unblock);
}

DeepgramCaptionStream::~DeepgramCaptionStream() {
    debug_log("~CaptionStream Deepgram destructor");
    if (!is_stopped())
        stop();

    int cleared = 0;
    {
        string *item;
        while (audio_queue.try_dequeue(item)) {
            delete item;
            cleared++;
        }
    }

    if (stream_thread) {
        stream_thread->detach();
        delete stream_thread;
        stream_thread = nullptr;
    }

    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
        curl_handle = nullptr;
    }

    debug_log("~CaptionStream Deepgram done, cleared %d from queue", cleared);
}
