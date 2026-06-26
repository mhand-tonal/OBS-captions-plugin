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

#ifndef OBS_CAPTION_STREAM_H
#define OBS_CAPTION_STREAM_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#endif

#include <functional>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <mutex>
#include <memory>
#include <ThreadsaferCallback.h>
#include <CaptionResult.h>

typedef unsigned int uint;
using namespace std;

typedef std::function<void(const CaptionResult &caption_result)> caption_text_callback;

enum SpeechApiProvider {
    SPEECH_API_GOOGLE_HTTP = 0,
    SPEECH_API_DEEPGRAM_WEBSOCKET = 1,
    SPEECH_API_LOCAL_WEBSOCKET = 2,
};

struct CaptionStreamSettings {
    uint connect_timeout_ms;
    uint send_timeout_ms;
    uint recv_timeout_ms;

    uint max_queue_depth;
    uint download_thread_start_delay_ms;

    string language;
    int profanity_filter;
    string api_key;
    string model;
    string keywords; // comma-separated boost terms
    string server_url; // ws:// URL for local WebSocket ASR sidecar

    CaptionStreamSettings(
            uint connect_timeout_ms,
            uint send_timeout_ms,
            uint recv_timeout_ms,

            uint max_queue_depth,
            uint download_thread_start_delay_ms,
            const string &language,
            int profanity_filter,
            const string &api_key,
            const string &model = "nova-3",
            const string &keywords = "",
            const string &server_url = "ws://localhost:6006"
    ) :
            connect_timeout_ms(connect_timeout_ms),
            send_timeout_ms(send_timeout_ms),
            recv_timeout_ms(recv_timeout_ms),

            max_queue_depth(max_queue_depth),
            download_thread_start_delay_ms(download_thread_start_delay_ms),
            language(language),
            profanity_filter(profanity_filter),
            api_key(api_key),
            model(model),
            keywords(keywords),
            server_url(server_url) {}

    bool operator==(const CaptionStreamSettings &rhs) const {
        return connect_timeout_ms == rhs.connect_timeout_ms &&
               send_timeout_ms == rhs.send_timeout_ms &&
               recv_timeout_ms == rhs.recv_timeout_ms &&
               max_queue_depth == rhs.max_queue_depth &&
               download_thread_start_delay_ms == rhs.download_thread_start_delay_ms &&
               language == rhs.language &&
               profanity_filter == rhs.profanity_filter &&
               api_key == rhs.api_key &&
               model == rhs.model &&
               keywords == rhs.keywords &&
               server_url == rhs.server_url;
    }

    bool operator!=(const CaptionStreamSettings &rhs) const {
        return !(rhs == *this);
    }

    void print(const char *line_prefix = "") {
        printf("%sCaptionStreamSettings\n", line_prefix);
        printf("%s  connect_timeout_ms: %d\n", line_prefix, connect_timeout_ms);
        printf("%s  send_timeout_ms: %d\n", line_prefix, send_timeout_ms);
        printf("%s  recv_timeout_ms: %d\n", line_prefix, recv_timeout_ms);
        printf("%s  max_queue_depth: %d\n", line_prefix, max_queue_depth);
        printf("%s  download_thread_start_delay_ms: %d\n", line_prefix, download_thread_start_delay_ms);
    }
};


class CaptionStream {
public:
    ThreadsaferCallback<caption_text_callback> on_caption_cb_handle;

    CaptionStream() = default;
    virtual ~CaptionStream() = default;

    virtual bool start(std::shared_ptr<CaptionStream> self) = 0;
    virtual void stop() = 0;
    virtual bool is_stopped() = 0;
    virtual bool is_started() = 0;
    virtual bool queue_audio_data(const char *data, const uint data_size) = 0;
};

#endif //OBS_CAPTION_STREAM_H
