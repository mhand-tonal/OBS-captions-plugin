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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cctype>

#include "CaptionStream.h"
#include "utils.h"
#include "log.h"
#include "../ws_common.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

#include <json11/json11.hpp>
using namespace json11;


// ---------------------------------------------------------------------------
// URL parser: ws://host[:port][/path]  (http:// and bare host also accepted)
// ---------------------------------------------------------------------------

static bool parse_ws_url(const string &url, string &host, int &port, string &path) {
    string s = url;

    if (s.find("ws://") == 0) s = s.substr(5);
    else if (s.find("wss://") == 0) s = s.substr(6);
    else if (s.find("http://") == 0) s = s.substr(7);
    else if (s.find("https://") == 0) s = s.substr(8);

    size_t slash = s.find('/');
    string hostport = (slash == string::npos) ? s : s.substr(0, slash);
    path = (slash == string::npos) ? "/" : s.substr(slash);
    if (path.empty()) path = "/";

    size_t colon = hostport.find(':');
    if (colon == string::npos) {
        host = hostport;
        port = 6006;
    } else {
        host = hostport.substr(0, colon);
        try {
            port = std::stoi(hostport.substr(colon + 1));
        } catch (...) {
            port = 6006;
        }
    }

    return !host.empty();
}


// ---------------------------------------------------------------------------
// CaptionStream – lifecycle
// ---------------------------------------------------------------------------

static std::once_flag local_ws_curl_init_flag;

LocalWebSocketCaptionStream::LocalWebSocketCaptionStream(CaptionStreamSettings settings)
        : settings(settings) {
    std::call_once(local_ws_curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        srand((unsigned) time(nullptr));
    });
    debug_log("CaptionStream local WebSocket created");
}

bool LocalWebSocketCaptionStream::start(std::shared_ptr<CaptionStream> self) {
    if (self.get() != this)
        return false;

    if (started)
        return false;

    started = true;
    stream_thread = new thread(&LocalWebSocketCaptionStream::stream_run, this, self);
    return true;
}

void LocalWebSocketCaptionStream::stream_run(std::shared_ptr<CaptionStream> self) {
    debug_log("starting local WS stream_run()");
    _stream_run();
    stop();
    debug_log("finished local WS stream_run()");
}


// ---------------------------------------------------------------------------
// CaptionStream – TCP connection via curl CONNECT_ONLY (plain, no TLS)
// ---------------------------------------------------------------------------

bool LocalWebSocketCaptionStream::curl_connect() {
    string url = settings.server_url.empty() ? "ws://localhost:6006" : settings.server_url;
    if (!parse_ws_url(url, host, port, path)) {
        error_log("[LocalWS] invalid server URL: %s", url.c_str());
        return false;
    }

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        error_log("[LocalWS] curl_easy_init failed");
        return false;
    }

    // Plain TCP via http:// scheme. sherpa-onnx's websocket server does not
    // use TLS on the path we're targeting, so we want a raw socket from curl.
    string connect_url = "http://" + host + ":" + std::to_string(port) + "/";

    curl_easy_setopt(curl_handle, CURLOPT_URL, connect_url.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, (long) settings.connect_timeout_ms);
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        error_log("[LocalWS] connect failed (%s): %s", connect_url.c_str(), curl_easy_strerror(res));
        return false;
    }

    res = curl_easy_getinfo(curl_handle, CURLINFO_ACTIVESOCKET, &sockfd);
    if (res != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        error_log("[LocalWS] failed to get socket from curl");
        return false;
    }

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sockfd, FIONBIO, &mode);
#else
    int flags = fcntl((int) sockfd, F_GETFL, 0);
    if (flags >= 0)
        fcntl((int) sockfd, F_SETFL, flags | O_NONBLOCK);
#endif

    debug_log("[LocalWS] TCP connected to %s:%d", host.c_str(), port);
    return true;
}


// ---------------------------------------------------------------------------
// CaptionStream – raw send / WebSocket framing
// ---------------------------------------------------------------------------

bool LocalWebSocketCaptionStream::send_all(const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        CURLcode rc = curl_easy_send(curl_handle, data + sent, len - sent, &n);
        if (rc == CURLE_OK) {
            sent += n;
        } else if (rc == CURLE_AGAIN) {
            if (ws_poll_socket(sockfd, POLLOUT, (int) settings.send_timeout_ms) <= 0)
                return false;
        } else {
            error_log("[LocalWS] send error: %s", curl_easy_strerror(rc));
            return false;
        }
    }
    return true;
}

bool LocalWebSocketCaptionStream::ws_send_frame(uint8_t opcode, const char *data, size_t len) {
    std::vector<uint8_t> frame;

    frame.push_back(0x80 | opcode);

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

    uint8_t mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (uint8_t)(rand() & 0xFF);
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < len; i++)
        frame.push_back(((uint8_t) data[i]) ^ mask[i % 4]);

    return send_all((const char *) frame.data(), frame.size());
}

bool LocalWebSocketCaptionStream::ws_send_binary(const char *data, size_t len) {
    return ws_send_frame(0x02, data, len);
}

bool LocalWebSocketCaptionStream::ws_send_text(const string &text) {
    return ws_send_frame(0x01, text.c_str(), text.size());
}


// ---------------------------------------------------------------------------
// CaptionStream – WebSocket handshake
// ---------------------------------------------------------------------------

bool LocalWebSocketCaptionStream::ws_handshake() {
    string ws_key = ws_generate_key();

    string use_path = path.empty() ? "/" : path;

    // sherpa-onnx's websocket server accepts any upgrade path; no auth, no query string.
    string request = "GET " + use_path + " HTTP/1.1\r\n"
                     "Host: " + host + ":" + std::to_string(port) + "\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: " + ws_key + "\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n";

    fprintf(stderr, "[LocalWS] handshake path=%s\n", use_path.c_str());
    fflush(stderr);

    if (!send_all(request.c_str(), request.size())) {
        error_log("[LocalWS] failed to send WebSocket upgrade request");
        return false;
    }

    string response;
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(settings.recv_timeout_ms);

    while (response.find("\r\n\r\n") == string::npos) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) {
            error_log("[LocalWS] WebSocket handshake timeout");
            return false;
        }

        ws_poll_socket(sockfd, POLLIN, (int) std::min(remaining_ms, (long long) 1000));

        char buf[4096];
        size_t nread = 0;
        CURLcode rc = curl_easy_recv(curl_handle, buf, sizeof(buf), &nread);
        if (rc == CURLE_OK && nread > 0) {
            response.append(buf, nread);
        } else if (rc != CURLE_AGAIN) {
            error_log("[LocalWS] handshake recv error: %s", curl_easy_strerror(rc));
            return false;
        }
    }

    if (response.find("HTTP/1.1 101") == string::npos) {
        size_t eol = response.find("\r\n");
        string status = (eol != string::npos) ? response.substr(0, eol) : response;
        error_log("[LocalWS] WebSocket upgrade failed: %s", status.c_str());
        return false;
    }

    size_t header_end = response.find("\r\n\r\n") + 4;
    if (header_end < response.size())
        recv_buffer = response.substr(header_end);

    debug_log("[LocalWS] WebSocket connected");
    return true;
}


// ---------------------------------------------------------------------------
// CaptionStream – emit a parsed result via the callback
// ---------------------------------------------------------------------------

void LocalWebSocketCaptionStream::emit_result(const string &text, bool is_final, const string &raw,
                                              const std::vector<CaptionWord> &words) {
    auto now = std::chrono::steady_clock::now();
    if (update_first_received_at) {
        first_received_at = now;
        update_first_received_at = false;
    }

    double stability = is_final ? 1.0 : 0.5;

    CaptionResult result(current_result_index, is_final, stability,
                         text, raw, first_received_at, now);
    result.speech_final = is_final;
    result.words = words;

    {
        std::lock_guard<recursive_mutex> lock(on_caption_cb_handle.mutex);
        if (on_caption_cb_handle.callback_fn)
            on_caption_cb_handle.callback_fn(result);
    }

    if (is_final) {
        update_first_received_at = true;
        current_result_index++;
    }
}


// ---------------------------------------------------------------------------
// CaptionStream – process incoming WebSocket frames
// ---------------------------------------------------------------------------
//
// Wire protocol (see tools/PROTOCOL.md):
//   Server emits text frames containing either bare transcript text (legacy
//   sherpa-onnx online-websocket-server) or JSON of the form:
//     {
//       "text":        "...",            // REQUIRED — current transcript
//       "segment":     0,                 // optional — increments per utterance
//       "is_endpoint": false,             // optional — true marks a final result
//       "words": [                        // optional — word-level timestamps (seconds)
//         {"word": "hello", "start": 0.10, "end": 0.42},
//         ...
//       ]
//     }
//   The literal string "Done!" closes the connection.
//
// Finalization precedence:
//   1. Explicit is_endpoint=true in the JSON payload (authoritative).
//   2. Heuristic: segment advanced since last message → previous interim was final.
//   3. Heuristic: (no segment field) new text is shorter than last → previous was final.
//
void LocalWebSocketCaptionStream::process_incoming() {
    WsFrame frame;
    while (try_parse_ws_frame(recv_buffer, frame)) {
        switch (frame.opcode) {
            case 0x01: { // text frame
                const string &payload = frame.payload;

                if (payload == "Done!") {
                    info_log("[LocalWS] server sent Done! — closing");
                    if (!last_interim_text.empty()) {
                        emit_result(last_interim_text, true, last_raw_message, {});
                        last_interim_text.clear();
                    }
                    ws_send_frame(0x08, nullptr, 0);
                    stop();
                    return;
                }

                string text;
                int segment = -1;
                bool explicit_endpoint = false;
                std::vector<CaptionWord> words;

                string err;
                Json json = Json::parse(payload, err);
                if (err.empty() && json.is_object()) {
                    text = json["text"].string_value();
                    if (json["segment"].is_number())
                        segment = (int) json["segment"].number_value();
                    if (json["is_endpoint"].is_bool())
                        explicit_endpoint = json["is_endpoint"].bool_value();

                    const Json &words_json = json["words"];
                    if (words_json.is_array()) {
                        for (const auto &w : words_json.array_items()) {
                            string wtext = w["word"].string_value();
                            if (wtext.empty())
                                wtext = w["text"].string_value();
                            double wstart = w["start"].number_value();
                            double wend = w["end"].number_value();
                            if (!wtext.empty())
                                words.emplace_back(std::move(wtext), wstart, wend);
                        }
                    }
                } else {
                    text = payload;
                }

                // Trim
                while (!text.empty() && std::isspace((unsigned char) text.front())) text.erase(text.begin());
                while (!text.empty() && std::isspace((unsigned char) text.back())) text.pop_back();

                // Case 1: explicit is_endpoint=true — emit as final immediately.
                if (explicit_endpoint) {
                    if (segment >= 0)
                        last_segment = segment;
                    if (!text.empty())
                        emit_result(text, true, payload, words);
                    else if (!last_interim_text.empty())
                        emit_result(last_interim_text, true, last_raw_message, words);
                    last_interim_text.clear();
                    last_raw_message.clear();
                    break;
                }

                // Case 2 & 3: heuristic finalization of the previous interim.
                bool segment_advanced = (segment >= 0 && last_segment >= 0 && segment != last_segment);
                bool shrunk = (segment < 0 && !last_interim_text.empty()
                               && text.size() < last_interim_text.size());

                if ((segment_advanced || shrunk) && !last_interim_text.empty()) {
                    emit_result(last_interim_text, true, last_raw_message, {});
                    last_interim_text.clear();
                }

                if (segment >= 0)
                    last_segment = segment;

                if (text.empty())
                    break;

                emit_result(text, false, payload, words);
                last_interim_text = text;
                last_raw_message = payload;
                break;
            }
            case 0x09: // ping → pong
                ws_send_frame(0x0A, frame.payload.c_str(), frame.payload.size());
                break;
            case 0x0A: // pong — ignore
                break;
            case 0x08: // close
                info_log("[LocalWS] server sent close frame");
                if (!last_interim_text.empty()) {
                    emit_result(last_interim_text, true, last_raw_message, {});
                    last_interim_text.clear();
                }
                ws_send_frame(0x08, nullptr, 0);
                stop();
                return;
            default:
                debug_log("[LocalWS] unknown WS opcode: 0x%02x", frame.opcode);
                break;
        }
    }
}


// ---------------------------------------------------------------------------
// CaptionStream – main event loop
// ---------------------------------------------------------------------------

void LocalWebSocketCaptionStream::_stream_run() {
    if (!curl_connect())
        return;

    if (is_stopped()) return;

    if (!ws_handshake())
        return;

    if (is_stopped()) return;

    debug_log("[LocalWS] stream active, entering event loop");

    while (!is_stopped()) {
        // 1. send all queued audio chunks (already float32 bytes)
        string *chunk;
        while (audio_queue.try_dequeue(chunk)) {
            if (!chunk->empty()) {
                if (!ws_send_binary(chunk->data(), chunk->size())) {
                    delete chunk;
                    error_log("[LocalWS] failed to send audio chunk");
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
                break;
            } else {
                if (rc == CURLE_OK)
                    info_log("[LocalWS] connection closed");
                else
                    error_log("[LocalWS] recv error: %s", curl_easy_strerror(rc));
                if (!last_interim_text.empty()) {
                    emit_result(last_interim_text, true, last_raw_message, {});
                    last_interim_text.clear();
                }
                return;
            }
        }

        if (got_data)
            process_incoming();

        ws_poll_socket(sockfd, POLLIN, 20);
    }

    // graceful close
    if (!last_interim_text.empty()) {
        emit_result(last_interim_text, true, last_raw_message, {});
        last_interim_text.clear();
    }
    ws_send_frame(0x08, nullptr, 0);
}


// ---------------------------------------------------------------------------
// CaptionStream – audio queue (int16 PCM → float32 conversion for sherpa-onnx)
// ---------------------------------------------------------------------------

bool LocalWebSocketCaptionStream::queue_audio_data(const char *audio_data, const uint data_size) {
    if (is_stopped())
        return false;

    // Incoming audio is 16-bit signed PCM, little-endian, mono, 16kHz (see
    // SourceCaptioner.cpp resample_info). sherpa-onnx's websocket server
    // expects float32 samples normalized to [-1.0, 1.0].
    const size_t sample_count = data_size / 2;
    string *str = new string();
    str->resize(sample_count * sizeof(float));

    const int16_t *in = reinterpret_cast<const int16_t *>(audio_data);
    float *out = reinterpret_cast<float *>(&(*str)[0]);
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sample_count; i++)
        out[i] = (float) in[i] * scale;

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
            debug_log("[LocalWS] queue overflow, dropped %d items", cleared_cnt);
    }

    audio_queue.enqueue(str);
    return true;
}

string *LocalWebSocketCaptionStream::dequeue_audio_data(const std::int64_t timeout_us) {
    string *ret;
    if (audio_queue.wait_dequeue_timed(ret, timeout_us))
        return ret;
    return nullptr;
}


// ---------------------------------------------------------------------------
// CaptionStream – stop / cleanup
// ---------------------------------------------------------------------------

bool LocalWebSocketCaptionStream::is_stopped() {
    return stopped;
}

bool LocalWebSocketCaptionStream::is_started() {
    return started;
}

void LocalWebSocketCaptionStream::stop() {
    info_log("[LocalWS] stop");
    on_caption_cb_handle.clear();
    stopped = true;

    string *to_unblock = new string();
    audio_queue.enqueue(to_unblock);
}

LocalWebSocketCaptionStream::~LocalWebSocketCaptionStream() {
    debug_log("~CaptionStream local WS destructor");
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

    debug_log("~CaptionStream local WS done, cleared %d from queue", cleared);
}
