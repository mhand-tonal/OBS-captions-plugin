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

#ifndef LOCAL_WEBSOCKET_CAPTIONSTREAM_H
#define LOCAL_WEBSOCKET_CAPTIONSTREAM_H

#include <vector>
#include <cameron314/blockingconcurrentqueue.h>
#include <CaptionStream.h>
#include <curl/curl.h>


class LocalWebSocketCaptionStream : public CaptionStream {
    CaptionStreamSettings settings;

    std::thread *stream_thread = nullptr;
    moodycamel::BlockingConcurrentQueue<string *> audio_queue;

    bool started = false;
    bool stopped = false;

    CURL *curl_handle = nullptr;
    curl_socket_t sockfd = CURL_SOCKET_BAD;
    string recv_buffer;

    // Parsed from settings.server_url at connect time
    string host;
    int port = 6006;
    string path = "/";

    // Transcript segmentation state
    int current_result_index = 0;
    int last_segment = -1;
    string last_interim_text;
    string last_raw_message;
    std::chrono::steady_clock::time_point first_received_at;
    bool update_first_received_at = true;

    string *dequeue_audio_data(const std::int64_t timeout_us);

    void stream_run(std::shared_ptr<CaptionStream> self);
    void _stream_run();

    bool curl_connect();
    bool ws_handshake();
    bool send_all(const char *data, size_t len);
    bool ws_send_frame(uint8_t opcode, const char *data, size_t len);
    bool ws_send_binary(const char *data, size_t len);
    bool ws_send_text(const string &text);
    void process_incoming();
    void emit_result(const string &text, bool is_final, const string &raw,
                     const std::vector<CaptionWord> &words);

public:

    LocalWebSocketCaptionStream(CaptionStreamSettings settings);

    bool start(std::shared_ptr<CaptionStream> self) override;

    void stop() override;

    bool is_connected() override;

    bool is_started() override;

    bool is_stopped() override;

    bool queue_audio_data(const char *data, const uint data_size) override;

    ~LocalWebSocketCaptionStream() override;
};

#endif //LOCAL_WEBSOCKET_CAPTIONSTREAM_H
