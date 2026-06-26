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

#ifndef GOOGLE_HTTP_CAPTIONSTREAM_H
#define GOOGLE_HTTP_CAPTIONSTREAM_H

#include "TcpConnection.h"
#include <queue>
#include <cameron314/blockingconcurrentqueue.h>
#include <CaptionStream.h>


class GoogleHttpCaptionStream : public CaptionStream {
    TcpConnection upstream;
    TcpConnection downstream;

    CaptionStreamSettings settings;
    string session_pair;

    std::thread *upstream_thread = nullptr;
    std::thread *downstream_thread = nullptr;

    moodycamel::BlockingConcurrentQueue<string *> audio_queue;

    bool started = false;
    bool stopped = false;

    string *dequeue_audio_data(const std::int64_t timeout_us);

    void upstream_run(std::shared_ptr<CaptionStream> self);

    void _upstream_run(std::shared_ptr<CaptionStream> self);


    void downstream_run(std::shared_ptr<CaptionStream> self);

    void _downstream_run();

public:

    GoogleHttpCaptionStream(
            CaptionStreamSettings settings
    );

    bool start(std::shared_ptr<CaptionStream> self) override;

    void stop() override;

    bool is_started() override;

    bool is_stopped() override;

    bool queue_audio_data(const char *data, const uint data_size) override;

    ~GoogleHttpCaptionStream() override;
};

#endif //GOOGLE_HTTP_CAPTIONSTREAM_H
