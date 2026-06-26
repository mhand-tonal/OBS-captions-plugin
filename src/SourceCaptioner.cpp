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

#include <memory>
#include <sstream>


#include "SourceCaptioner.h"
#include "log.c"
#include "caption_output_writer.h"
#include "caption_transcript_writer.h"

typedef std::tuple<string, string> TextOutputTup;

static void keep_last_lines(vector<string> &lines, uint keep);

void set_text_source_text(const string &text_source_name, const string &caption_text) {
    obs_source_t *text_source = obs_get_source_by_name(text_source_name.c_str());
    if (!text_source) {
//        warn_log("text source: %s not found, can't set caption text", text_source_name.c_str());
        return;
    }
//    debug_log("set_text_source_text '%s': '%s'", text_source_name.c_str(), caption_text.c_str());

    obs_data_t *text_settings = obs_data_create();
    obs_data_set_string(text_settings, "text", caption_text.c_str());
    obs_source_update(text_source, text_settings);
    obs_data_release(text_settings);

    obs_source_release(text_source);
}

SourceCaptioner::SourceCaptioner(const bool enabled, const SourceCaptionerSettings &settings, const string &scene_collection_name, bool start) :
        QObject(),
        base_enabled(enabled),
        settings(settings),
        selected_scene_collection_name(scene_collection_name),
        last_caption_at(std::chrono::steady_clock::now()),
        last_caption_cleared(true) {

    QObject::connect(&timer, &QTimer::timeout, this, &SourceCaptioner::clear_output_timer_cb);
    QObject::connect(&word_reveal_timer, &QTimer::timeout, this, &SourceCaptioner::word_reveal_timer_cb);

    QObject::connect(this, &SourceCaptioner::received_caption_result,
                     this, &SourceCaptioner::process_caption_result, Qt::QueuedConnection);

    QObject::connect(this, &SourceCaptioner::audio_capture_status_changed,
                     this, &SourceCaptioner::process_audio_capture_status_change);

    timer.start(1000);

    const SceneCollectionSettings &scene_col_settings = this->settings.get_scene_collection_settings(scene_collection_name);
    debug_log("SourceCaptioner, source '%s'", scene_col_settings.caption_source_settings.caption_source_name.c_str());

    if (start)
        start_caption_stream(settings, scene_collection_name);
}


void SourceCaptioner::stop_caption_stream(bool send_signal) {
    if (!send_signal) {
        std::lock_guard<recursive_mutex> lock(settings_change_mutex);
        source_audio_capture_session = nullptr;
        output_audio_capture_session = nullptr;
        caption_result_handler = nullptr;
        continuous_captions = nullptr;
        audio_capture_id++;
        return;
    }

    settings_change_mutex.lock();

    SourceCaptionerSettings cur_settings = settings;
    string cur_scene_collection_name = this->selected_scene_collection_name;

    source_audio_capture_session = nullptr;
    output_audio_capture_session = nullptr;
    caption_result_handler = nullptr;
    continuous_captions = nullptr;
    audio_capture_id++;

    settings_change_mutex.unlock();

    if (send_signal) {
        emit source_capture_status_changed(std::make_shared<SourceCaptionerStatus>(
                SOURCE_CAPTIONER_STATUS_EVENT_STOPPED,
                false,
                false,
                cur_settings,
                cur_scene_collection_name,
                AUDIO_SOURCE_NOT_STREAMED,
                false
        ));
    }
}

bool SourceCaptioner::set_settings(const SourceCaptionerSettings &new_settings, const string &scene_collection_name) {
//    debug_log("SourceCaptioner::set_settings");

    bool settings_equal;
    bool stream_settings_equal;
    {
        std::lock_guard<recursive_mutex> lock(settings_change_mutex);
        stop_caption_stream(false);

        settings_equal = settings == new_settings;
        stream_settings_equal = settings.stream_settings == new_settings.stream_settings;

        settings = new_settings;
        selected_scene_collection_name = scene_collection_name;

        fileoutput_captions_output.clear();
    }

    emit source_capture_status_changed(std::make_shared<SourceCaptionerStatus>(
            SOURCE_CAPTIONER_STATUS_EVENT_NEW_SETTINGS_STOPPED,
            !settings_equal,
            !stream_settings_equal,
            new_settings,
            scene_collection_name,
            AUDIO_SOURCE_NOT_STREAMED,
            false
    ));

    return true;
}

bool SourceCaptioner::start_caption_stream(const SourceCaptionerSettings &new_settings, const string &scene_collection_name) {
//    debug_log("start_caption_stream %d", (int) std::hash<std::thread::id>{}(std::this_thread::get_id()));
    bool started_ok;
    bool settings_equal;
    bool stream_settings_equal;
    audio_source_capture_status audio_cap_status = AUDIO_SOURCE_NOT_STREAMED;

    {
        std::lock_guard<recursive_mutex> lock(settings_change_mutex);
        settings_equal = settings == new_settings;
        stream_settings_equal = settings.stream_settings == new_settings.stream_settings;

        settings = new_settings;
        selected_scene_collection_name = scene_collection_name;

        source_audio_capture_session = nullptr;
        output_audio_capture_session = nullptr;
        caption_result_handler = nullptr;
        audio_capture_id++;

        started_ok = _start_caption_stream(!stream_settings_equal);

        if (!started_ok)
            stop_caption_stream(false);

        if (started_ok) {
            if (source_audio_capture_session)
                audio_cap_status = source_audio_capture_session->get_current_capture_status();
            else if (output_audio_capture_session)
                audio_cap_status = AUDIO_SOURCE_CAPTURING;
            else {
                stop_caption_stream(false);
                started_ok = false;
            }
            if (started_ok) {
                if (new_settings.file_output_settings.isValidEnabled()) {
                    fileoutput_captions_output.clear();

                    FileOutputSettings fsets = new_settings.file_output_settings;
                    auto control = std::make_shared<CaptionOutputControl<FileOutputSettings>>(fsets);
                    fileoutput_captions_output.set_control(control);
                    std::thread th(fileoutput_writer_loop, control, fsets);
                    th.detach();
                }
            }
        }
    }

    if (started_ok) {
//        info_log("start_caption_stream OK, %d ", audio_cap_status);
        emit source_capture_status_changed(std::make_shared<SourceCaptionerStatus>(
                SOURCE_CAPTIONER_STATUS_EVENT_STARTED_OK,
                !settings_equal,
                !stream_settings_equal,
                new_settings,
                scene_collection_name,
                audio_cap_status,
                true
        ));
    } else {
//        info_log("start_caption_stream FAIL");
        emit source_capture_status_changed(std::make_shared<SourceCaptionerStatus>(
                SOURCE_CAPTIONER_STATUS_EVENT_STARTED_ERROR,
                !settings_equal,
                !stream_settings_equal,
                new_settings,
                scene_collection_name,
                AUDIO_SOURCE_NOT_STREAMED,
                false
        ));
    }

    return started_ok;
}

bool SourceCaptioner::_start_caption_stream(bool restart_stream) {
//    debug_log("start_caption_stream");

    bool caption_settings_equal;
    {
        const SceneCollectionSettings &scene_col_settings = this->settings.get_scene_collection_settings(selected_scene_collection_name);
        const CaptionSourceSettings &selected_caption_source_settings = scene_col_settings.caption_source_settings;

        debug_log("SourceCaptioner start_caption_stream, source '%s'", selected_caption_source_settings.caption_source_name.c_str());

        if (selected_caption_source_settings.caption_source_name.empty()) {
            warn_log("SourceCaptioner start_caption_stream, empty source given.");
            return false;
        }

        const bool use_output_audio = is_all_audio_output_capture_source_data(selected_caption_source_settings.caption_source_name);

        OBSSource caption_source;
        OBSSource mute_source;

        if (!use_output_audio) {
            caption_source = obs_get_source_by_name(selected_caption_source_settings.caption_source_name.c_str());
            obs_source_release(caption_source);
            if (!caption_source) {
                warn_log("SourceCaptioner start_caption_stream, no caption source with name: '%s'",
                         selected_caption_source_settings.caption_source_name.c_str());
                return false;
            }

            if (selected_caption_source_settings.mute_when == CAPTION_SOURCE_MUTE_TYPE_USE_OTHER_MUTE_SOURCE) {
                mute_source = obs_get_source_by_name(selected_caption_source_settings.mute_source_name.c_str());
                obs_source_release(mute_source);

                if (!mute_source) {
                    warn_log("SourceCaptioner start_caption_stream, no mute source with name: '%s'",
                             selected_caption_source_settings.mute_source_name.c_str());
                    return false;
                }
            }
        }

        debug_log("caption_settings_equal: %d, %d", caption_settings_equal, continuous_captions != nullptr);
        if (!continuous_captions || restart_stream) {
            try {

                ContinuousCaptionStreamSettings settings_copy = settings.stream_settings;
                debug_log("using api key length %lu", settings_copy.stream_settings.api_key.length());
                info_log("provider=%d model='%s' keywords='%s'",
                         settings_copy.provider,
                         settings_copy.stream_settings.model.c_str(),
                         settings_copy.stream_settings.keywords.c_str());

                // Pre-emptive connection cycling is only needed for Google
                // Cloud Speech, which has a 5-minute hard session cap. Every
                // other provider (Deepgram, Local WebSocket) keeps a single
                // connection open for the full session — cycling would
                // introduce avoidable reconnect churn and model re-warmup.
                if (settings_copy.provider != SPEECH_API_GOOGLE_HTTP) {
                    settings_copy.connect_second_after_secs = 0;
                    settings_copy.switchover_second_after_secs = 0;
                }

                auto caption_cb = std::bind(&SourceCaptioner::on_caption_text_callback, this, std::placeholders::_1, std::placeholders::_2);
                continuous_captions = std::make_unique<ContinuousCaptions>(settings_copy);
                continuous_captions->on_caption_cb_handle.set(caption_cb, true);
            }
            catch (...) {
                warn_log("couldn't create ContinuousCaptions");
                return false;
            }
        }
        // Reset caption mode state for fresh start
        append_only_last_result_index = -1;
        append_only_last_result_started_at = {};
        reset_caption_mode_state();

        caption_result_handler = std::make_unique<CaptionResultHandler>(settings.format_settings);

        try {
            resample_info resample_to = {16000, AUDIO_FORMAT_16BIT, SPEAKERS_MONO};
            audio_chunk_data_cb audio_cb = std::bind(&SourceCaptioner::on_audio_data_callback, this,
                                                     std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

            auto audio_status_cb = std::bind(&SourceCaptioner::on_audio_capture_status_change_callback, this,
                                             std::placeholders::_1, std::placeholders::_2);

            if (use_output_audio) {
                int track_index = all_audio_output_capture_source_track_index(selected_caption_source_settings.caption_source_name);
                if (track_index < 0)
                    track_index = 0;

                output_audio_capture_session = std::make_unique<OutputAudioCaptureSession>(track_index,
                                                                                           audio_cb, audio_status_cb,
                                                                                           resample_to,
                                                                                           audio_capture_id);
            } else {
                source_audio_capture_session = std::make_unique<SourceAudioCaptureSession>(caption_source, mute_source, audio_cb,
                                                                                           audio_status_cb,
                                                                                           resample_to,
                                                                                           MUTED_SOURCE_REPLACE_WITH_ZERO,
                                                                                           false,
                                                                                           audio_capture_id);
            }


        }
        catch (std::string err) {
            warn_log("couldn't create AudioCaptureSession, %s", err.c_str());
            return false;
        }
        catch (...) {
            warn_log("couldn't create AudioCaptureSession");
            return false;
        }

    }
//    debug_log("started captioning source '%s'", selected_caption_source_settings.caption_source_name.c_str());
//    debug_log("started captioning source tid '%d'", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return true;
}

void SourceCaptioner::on_audio_capture_status_change_callback(const int id, const audio_source_capture_status status) {
//    debug_log("capture status change %d %d", status, (int) std::hash<std::thread::id>{}(std::this_thread::get_id()));
    emit audio_capture_status_changed(id, status);
}


void SourceCaptioner::process_audio_capture_status_change(const int cb_audio_capture_id, const int new_status) {
//    debug_log("process_audio_capture_status_change %d %d", new_status, (int) std::hash<std::thread::id>{}(std::this_thread::get_id()));

    settings_change_mutex.lock();

    bool is_old_audio_session = cb_audio_capture_id != audio_capture_id;
    SourceCaptionerSettings cur_settings = settings;
    string cur_scene_collection_name = selected_scene_collection_name;
    bool active = continuous_captions != nullptr;

    settings_change_mutex.unlock();

    if (is_old_audio_session) {
        debug_log("ignoring old audio capture status!!");
        return;
    }

    emit source_capture_status_changed(std::make_shared<SourceCaptionerStatus>(
            SOURCE_CAPTIONER_STATUS_EVENT_AUDIO_CAPTURE_STATUS_CHANGE,
            false,
            false,
            cur_settings,
            cur_scene_collection_name,
            (audio_source_capture_status) new_status,
            active
    ));
}


void SourceCaptioner::on_audio_data_callback(const int id, const uint8_t *data, const size_t size) {
//    info_log("audio data");
    if (continuous_captions) {
        // safe without locking as continuous_captions only ever gets updated when there's no AudioCaptureSession running
        continuous_captions->queue_audio_data((char *) data, size);
    }
    audio_chunk_count++;

}

void SourceCaptioner::word_reveal_timer_cb() {
    std::lock_guard<recursive_mutex> lock(settings_change_mutex);

    const uint line_length = settings.format_settings.caption_line_length;
    const uint line_count = settings.format_settings.caption_line_count;
    const bool is_subtitle_box = (settings.format_settings.stream_caption_output_mode == STREAM_OUTPUT_MODE_SUBTITLE_BOX);
    const double dwell_secs = settings.format_settings.card_dwell_seconds;

    // If dwelling, check expiry
    if (card_dwelling) {
        auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - card_dwell_start).count();
        if (elapsed < dwell_secs)
            return; // keep ticking, keep card on screen
        card_dwelling = false;
        growing_committed_lines.clear();
        growing_current_line.clear();
    }

    if (word_reveal_queue.empty()) {
        word_reveal_timer.stop();
        return;
    }

    const string &word = word_reveal_queue.front();
    size_t proposed = growing_current_line.empty()
                      ? word.size()
                      : growing_current_line.size() + 1 + word.size();
    if (!growing_current_line.empty() && proposed > line_length) {
        if (is_subtitle_box && (int)growing_committed_lines.size() >= (int)line_count - 1) {
            if (dwell_secs > 0) {
                // Don't pop — hold the full card on screen for dwell_secs.
                card_dwelling = true;
                card_dwell_start = std::chrono::steady_clock::now();
                return;
            }
            growing_committed_lines.clear();
            growing_current_line.clear();
        } else {
            growing_committed_lines.push_back(std::move(growing_current_line));
            growing_current_line.clear();
            if (!is_subtitle_box) {
                const uint keep = line_count > 1 ? line_count - 1 : 0;
                keep_last_lines(growing_committed_lines, keep);
            }
        }
    }
    if (!growing_current_line.empty()) growing_current_line += ' ';
    growing_current_line += word;
    word_reveal_queue.pop_front();

    if (word_reveal_queue.empty() && !card_dwelling)
        word_reveal_timer.stop();

    CaptionResult dummy_cr;
    dummy_cr.final = false;
    auto result = build_word_reveal_display(dummy_cr, false, line_count, is_subtitle_box);
    if (result->output_lines.empty() && !is_subtitle_box)
        return;

    bool to_stream = settings.streaming_output_enabled;
    bool to_recording = settings.recording_output_enabled;

    if (to_stream || to_recording) {
        this->output_caption_writers(
                CaptionOutput(result, false),
                to_stream, to_recording,
                false, false, false, false);
        caption_was_output();
    }

    // Update preview
    string recent_text;
    prepare_recent(recent_text);
    emit caption_result_received(result, false, recent_text);
}

void SourceCaptioner::clear_output_timer_cb() {
//    info_log("clear timer checkkkkkkkkkkkkkkk");

    bool to_stream, to_recording, to_transcript_streaming, to_transcript_recording;
    vector<string> text_source_names;
    bool clear_fileoutput = false;
    {
        std::lock_guard<recursive_mutex> lock(settings_change_mutex);
        if (!this->settings.format_settings.caption_timeout_enabled || this->last_caption_cleared)
            return;

        double secs_since_last_caption = std::chrono::duration_cast<std::chrono::duration<double >>(
                std::chrono::steady_clock::now() - this->last_caption_at).count();

        if (secs_since_last_caption <= this->settings.format_settings.caption_timeout_seconds)
            return;

        info_log("last caption line was sent %f secs ago, > %f, clearing",
                 secs_since_last_caption, this->settings.format_settings.caption_timeout_seconds);

        this->last_caption_cleared = true;
        reset_caption_mode_state();
        word_reveal_timer.stop();
        to_stream = settings.streaming_output_enabled;
        to_recording = settings.recording_output_enabled;
        to_transcript_streaming = settings.transcript_settings.enabled && settings.transcript_settings.streaming_transcripts_enabled;
        to_transcript_recording = settings.transcript_settings.enabled && settings.transcript_settings.recording_transcripts_enabled;

        const SceneCollectionSettings &scene_col_settings = this->settings.get_scene_collection_settings(selected_scene_collection_name);
        for (const auto &text_out: scene_col_settings.text_outputs) {
            if (!text_out.isValidEnabled())
                continue;
            text_source_names.push_back(text_out.text_source_name);
        }

        if (fileoutput_captions_output.control) {
            clear_fileoutput = true;
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto clearance = CaptionOutput(std::make_shared<OutputCaptionResult>(CaptionResult(0, false, 0, "", "", now, now), false), true);
    output_caption_writers(clearance,
                           to_stream,
                           to_recording,
                           false,
                           false,
                           false,
                           true);

    for (const auto &to_clear: text_source_names) {
        set_text_source_text(to_clear, " ");
    }

    if (clear_fileoutput) {
        fileoutput_captions_output.enqueue(clearance);
    }

    emit caption_result_received(nullptr, true, "");
}


void SourceCaptioner::store_result(shared_ptr<OutputCaptionResult> output_result) {
    if (!output_result)
        return;

    if (output_result->caption_result.final) {
        results_history.push_back(output_result);
        held_nonfinal_caption_result = nullptr;
        debug_log("final, adding to history: %d %s", (int) results_history.size(), output_result->clean_caption_text.c_str());
    } else {
        held_nonfinal_caption_result = output_result;
    }

    if (results_history.size() > HISTORY_ENTRIES_HIGH_WM) {
        results_history.erase(results_history.begin(), results_history.begin() + HISTORY_ENTRIES_LOW_WM);
//        debug_log("cleaning result history done %d", (int) results_history.size());
    }
}


void SourceCaptioner::prepare_recent(string &recent_captions_output) {
    for (auto i = results_history.rbegin(); i != results_history.rend(); ++i) {
        if (!(*i))
            break;

        if (!(*i)->caption_result.final)
            break;

        if (recent_captions_output.size() + (*i)->clean_caption_text.size() >= MAX_HISTORY_VIEW_LENGTH)
            break;

        if ((*i)->clean_caption_text.empty())
            continue;

        if (recent_captions_output.empty()) {
            recent_captions_output.insert(0, 1, '.');
        } else {
            recent_captions_output.insert(0, ". ");
        }

        recent_captions_output.insert(0, (*i)->clean_caption_text);
    }

    if (held_nonfinal_caption_result) {
        if (!recent_captions_output.empty())
            recent_captions_output.push_back(' ');
        recent_captions_output.append("    >> ");
        recent_captions_output.append(held_nonfinal_caption_result->clean_caption_text);
    }
}

void SourceCaptioner::on_caption_text_callback(const CaptionResult &caption_result, bool interrupted) {
    // emit qt signal to avoid possible thread deadlock
    // this callback comes from the captioner thread, result processing needs settings_change_mutex, so does clearing captioner,
    // but that waits for the captioner callback to finish which might be waiting on the lock otherwise.

    emit received_caption_result(caption_result, interrupted);
}

static vector<string> split_words(const string &text) {
    vector<string> out;
    std::istringstream ss(text);
    string w;
    while (std::getline(ss, w, ' ')) {
        if (!w.empty()) out.push_back(std::move(w));
    }
    return out;
}

static size_t word_boundary_after(const string &text, size_t word_count) {
    size_t pos = 0;
    size_t seen_words = 0;
    while (pos < text.size() && seen_words < word_count) {
        while (pos < text.size() && text[pos] == ' ')
            pos++;
        while (pos < text.size() && text[pos] != ' ')
            pos++;
        seen_words++;
    }
    return pos;
}

static string compute_rebased_append_delta(const string &previous_text, const string &current_text) {
    vector<string> previous_words = split_words(previous_text);
    vector<string> current_words = split_words(current_text);

    size_t common_words = 0;
    const size_t max_common_words = previous_words.size() < current_words.size()
                                    ? previous_words.size()
                                    : current_words.size();
    while (common_words < max_common_words &&
           previous_words[common_words] == current_words[common_words]) {
        common_words++;
    }

    const size_t delta_start = common_words == 0 ? 0 : word_boundary_after(current_text, common_words);
    return current_text.substr(delta_start);
}

static void keep_last_lines(vector<string> &lines, uint keep) {
    if (lines.size() > keep)
        lines.erase(lines.begin(), lines.end() - keep);
}

bool SourceCaptioner::should_skip_nonfinal_for_stability(const CaptionResult &cr) const {
    return !cr.final && cr.stability < settings.format_settings.append_stability_threshold;
}

bool SourceCaptioner::should_skip_nonfinal_for_debounce(const CaptionResult &cr,
                                                       std::chrono::steady_clock::time_point now) const {
    if (cr.final) return false;
    auto since_last = std::chrono::duration_cast<std::chrono::duration<double>>(
            now - last_stream_caption_sent_at).count();
    return since_last < settings.format_settings.debounce_delay_seconds;
}

shared_ptr<OutputCaptionResult> SourceCaptioner::build_word_reveal_display(
        const CaptionResult &cr, bool interrupted, uint line_count, bool is_subtitle_box) {
    const size_t total = growing_committed_lines.size() + (growing_current_line.empty() ? 0 : 1);
    auto result = std::make_shared<OutputCaptionResult>(cr, interrupted);
    if (total == 0 && !is_subtitle_box) {
        result->output_line.clear();
        return result;
    }

    const size_t target = is_subtitle_box && total < line_count ? line_count : total;
    string display;
    vector<string> display_lines;
    display_lines.reserve(target);

    for (const auto &l : growing_committed_lines) {
        if (!display.empty()) display += "\r\n";
        display += l;
        display_lines.push_back(l);
    }
    if (!growing_current_line.empty()) {
        if (!display.empty()) display += "\r\n";
        display += growing_current_line;
        display_lines.push_back(growing_current_line);
    }
    while (is_subtitle_box && display_lines.size() < line_count) {
        if (!display.empty()) display += "\r\n";
        display += ' ';
        display_lines.emplace_back(" ");
    }

    result->clean_caption_text = display;
    result->output_line = display;
    result->output_lines = std::move(display_lines);
    return result;
}

void SourceCaptioner::reset_caption_mode_state() {
    append_only_sent_text.clear();
    growing_committed_lines.clear();
    growing_current_line.clear();
    word_reveal_queue.clear();
    lowlatency_buffering = false;
    card_dwelling = false;
    segment_committed_words = 0;
    segment_index = -1;
    segment_buffered_text.clear();
    segment_buffered_count = 0;
    segment_buffer_start = {};
}

string SourceCaptioner::compute_append_delta(const CaptionResult &cr, const string &current_text) {
    // When a new segment/utterance starts (index or timestamp changes), clear
    // the delta tracker so the new text is treated as fresh delta.  Do NOT reset
    // the display state (growing lines, subtitle box) — text should accumulate
    // continuously across segments and only clear when the box is full or on
    // explicit restart.
    if (cr.index != append_only_last_result_index ||
        cr.first_received_at != append_only_last_result_started_at) {
        append_only_last_result_index = cr.index;
        append_only_last_result_started_at = cr.first_received_at;
        append_only_sent_text.clear();
    }

    string delta;
    bool should_rebaseline = false;
    if (current_text.size() > append_only_sent_text.size() &&
        current_text.compare(0, append_only_sent_text.size(), append_only_sent_text) == 0) {
        delta = current_text.substr(append_only_sent_text.size());
        should_rebaseline = true;
    } else {
        const bool pure_backtrack =
                append_only_sent_text.size() > current_text.size() &&
                append_only_sent_text.compare(0, current_text.size(), current_text) == 0;
        if (!current_text.empty() && !pure_backtrack && current_text != append_only_sent_text) {
            delta = compute_rebased_append_delta(append_only_sent_text, current_text);
            should_rebaseline = true;
        }
    }

    if (should_rebaseline) {
        static constexpr size_t APPEND_TRACKER_MAX = 64 * 1024;
        if (current_text.size() > APPEND_TRACKER_MAX)
            append_only_sent_text.clear();
        else
            append_only_sent_text = current_text;
    }
    return delta;
}

void SourceCaptioner::process_caption_result(const CaptionResult caption_result, bool interrupted) {
    shared_ptr<OutputCaptionResult> native_output_result;
    shared_ptr<OutputCaptionResult> file_output_result(nullptr);
    shared_ptr<OutputCaptionResult> stream_output_result;
    string recent_caption_text;
    bool to_stream, to_recording, to_transcript_streaming, to_transcript_recording, to_transcript_virtualcam;
    bool preview_this_result = true;

    if (this->last_caption_text == caption_result.caption_text && this->last_caption_final == caption_result.final) {
        return;
    }
    this->last_caption_text = caption_result.caption_text;
    this->last_caption_final = caption_result.final;

    vector<TextOutputTup> text_source_sets;
    {
        std::lock_guard<recursive_mutex> lock(settings_change_mutex);

        if (!caption_result_handler) {
            warn_log("no caption_result_handler, shouldn't happen, there should be no AudioCaptureSession running");
            return;
        }

        native_output_result = caption_result_handler->prepare_caption_output(caption_result,
                                                                              true,
                                                                              settings.format_settings.caption_insert_newlines,
                                                                              settings.format_settings.caption_insert_punctuation,
                                                                              settings.format_settings.caption_line_length,
                                                                              settings.format_settings.caption_line_count,
                                                                              settings.format_settings.capitalization,
                                                                              interrupted,
                                                                              results_history);
        if (!native_output_result)
            return;

        const SceneCollectionSettings &scene_col_settings = this->settings.get_scene_collection_settings(selected_scene_collection_name);
        for (const auto &text_out: scene_col_settings.text_outputs) {
            if (!text_out.isValidEnabled())
                continue;

            auto text_output_result = caption_result_handler->prepare_caption_output(caption_result,
                                                                                     true,
                                                                                     true,
                                                                                     text_out.insert_punctuation,
                                                                                     text_out.line_length,
                                                                                     text_out.line_count,
                                                                                     text_out.capitalization,
                                                                                     interrupted,
                                                                                     results_history);

            text_source_sets.emplace_back(text_out.text_source_name, text_output_result->output_line);
        }

        store_result(native_output_result);

//        info_log("got caption '%s'", native_output_result->clean_caption_text.c_str());
//        info_log("output line '%s'", output_caption_line.c_str());

        prepare_recent(recent_caption_text);

        to_stream = settings.streaming_output_enabled;
        to_recording = settings.recording_output_enabled;

        if (to_stream || to_recording) {
            const auto now = std::chrono::steady_clock::now();
            const string &current_text = native_output_result->clean_caption_text;
            const uint line_length = settings.format_settings.caption_line_length;
            const uint line_count = settings.format_settings.caption_line_count;
            const auto skip_output = [&]() {
                to_stream = false;
                to_recording = false;
                preview_this_result = false;
            };

            switch (settings.format_settings.stream_caption_output_mode) {
                case STREAM_OUTPUT_MODE_DEBOUNCED:
                    if (should_skip_nonfinal_for_debounce(caption_result, now))
                        skip_output();
                    break;
                case STREAM_OUTPUT_MODE_APPEND_ONLY: {
                    if (should_skip_nonfinal_for_stability(caption_result) ||
                        should_skip_nonfinal_for_debounce(caption_result, now)) {
                        skip_output();
                        break;
                    }
                    string delta = compute_append_delta(caption_result, current_text);
                    if (delta.empty()) {
                        skip_output();
                        break;
                    }
                    auto delta_result = std::make_shared<OutputCaptionResult>(caption_result, interrupted);
                    delta_result->clean_caption_text = delta;
                    delta_result->output_line = delta;
                    delta_result->output_lines.push_back(std::move(delta));
                    stream_output_result = delta_result;
                    break;
                }
                case STREAM_OUTPUT_MODE_GROWING:
                case STREAM_OUTPUT_MODE_SUBTITLE_BOX: {
                    {
                        // Shared word intake: append-only word counting with delay buffer.
                        // Both interims and finals can add words, but only NEW words
                        // (beyond what's already committed). Revisions are ignored.
                        // Line breaks are permanent: once a word is placed, it never moves.
                        const int reveal_ms = settings.format_settings.word_reveal_delay_ms;
                        const double delay_secs = settings.format_settings.debounce_delay_seconds;
                        const bool is_subtitle_box = (settings.format_settings.stream_caption_output_mode == STREAM_OUTPUT_MODE_SUBTITLE_BOX);

                        vector<string> current_words = split_words(current_text);

                        if (caption_result.index != segment_index ||
                            caption_result.first_received_at != segment_started_at) {
                            if (segment_buffered_count > segment_committed_words) {
                                vector<string> flush_words = split_words(segment_buffered_text);
                                for (int i = segment_committed_words; i < (int)flush_words.size(); i++)
                                    word_reveal_queue.push_back(std::move(flush_words[i]));
                            }

                            // New segment may repeat the last few words from the previous
                            // one (ASR context carryover). Find the longest suffix of
                            // the display tail that matches a prefix of the new segment
                            // (up to 3 words, case-insensitive, punctuation-stripped).
                            vector<string> tail_words;
                            if (!growing_current_line.empty())
                                for (auto &w : split_words(growing_current_line))
                                    tail_words.push_back(w);
                            for (auto &w : word_reveal_queue)
                                tail_words.push_back(w);

                            auto normalize = [](const string &s) {
                                string r = s;
                                while (!r.empty() && std::ispunct((unsigned char)r.back()))
                                    r.pop_back();
                                for (auto &c : r) c = std::tolower((unsigned char)c);
                                return r;
                            };

                            int overlap = 0;
                            if (!tail_words.empty() && !current_words.empty()) {
                                int max_check = std::min({(int)tail_words.size(), (int)current_words.size(), 3});
                                for (int len = 1; len <= max_check; len++) {
                                    bool match = true;
                                    for (int j = 0; j < len; j++) {
                                        if (normalize(tail_words[tail_words.size() - len + j]) !=
                                            normalize(current_words[j])) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    if (match) overlap = len;
                                }
                            }

                            segment_committed_words = overlap;
                            segment_buffered_count = 0;
                            segment_buffered_text.clear();
                            segment_buffer_start = {};
                            segment_index = caption_result.index;
                            segment_started_at = caption_result.first_received_at;
                        }

                        const int word_count = (int)current_words.size();
                        if (word_count > segment_buffered_count) {
                            if (segment_buffered_count <= segment_committed_words)
                                segment_buffer_start = now;
                            segment_buffered_count = word_count;
                        }
                        segment_buffered_text = current_text;

                        bool should_commit = false;
                        if (caption_result.final) {
                            should_commit = true;
                        } else if (segment_buffered_count > segment_committed_words &&
                                   segment_buffer_start != std::chrono::steady_clock::time_point{}) {
                            auto waited = std::chrono::duration_cast<std::chrono::duration<double>>(
                                    now - segment_buffer_start).count();
                            should_commit = (waited >= delay_secs);
                        }

                        if (should_commit && segment_buffered_count > segment_committed_words) {
                            for (int i = segment_committed_words; i < (int)current_words.size(); i++)
                                word_reveal_queue.push_back(std::move(current_words[i]));
                            segment_committed_words = word_count;
                            segment_buffer_start = {};
                        }

                        const double dwell_secs = is_subtitle_box ? settings.format_settings.card_dwell_seconds : 0.0;

                        // If dwelling (subtitle box card full, waiting before clear), check expiry
                        if (card_dwelling) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                                    now - card_dwell_start).count();
                            if (elapsed < dwell_secs) {
                                // Re-emit current card so native ASR text doesn't leak through.
                                stream_output_result = build_word_reveal_display(
                                        caption_result, interrupted, line_count, is_subtitle_box);
                                break;
                            }
                            card_dwelling = false;
                            growing_committed_lines.clear();
                            growing_current_line.clear();
                        }

                        if (reveal_ms <= 0) {
                            while (!word_reveal_queue.empty()) {
                                const string &word = word_reveal_queue.front();
                                size_t proposed = growing_current_line.empty()
                                                  ? word.size()
                                                  : growing_current_line.size() + 1 + word.size();
                                if (!growing_current_line.empty() && proposed > line_length) {
                                    if ((int)growing_committed_lines.size() >= (int)line_count - 1) {
                                        if (is_subtitle_box) {
                                            if (dwell_secs > 0) {
                                                card_dwelling = true;
                                                card_dwell_start = now;
                                                break;
                                            }
                                            growing_committed_lines.clear();
                                            growing_current_line.clear();
                                        } else {
                                            growing_committed_lines.push_back(std::move(growing_current_line));
                                            growing_current_line.clear();
                                            const uint keep = line_count > 1 ? line_count - 1 : 0;
                                            keep_last_lines(growing_committed_lines, keep);
                                        }
                                    } else {
                                        growing_committed_lines.push_back(std::move(growing_current_line));
                                        growing_current_line.clear();
                                    }
                                }
                                if (!growing_current_line.empty()) growing_current_line += ' ';
                                growing_current_line += word;
                                word_reveal_queue.pop_front();
                            }
                        }

                        if (!word_reveal_queue.empty() && reveal_ms > 0 && !word_reveal_timer.isActive()) {
                            word_reveal_timer.start(std::max(reveal_ms, 20));
                        } else if (word_reveal_queue.empty() && !card_dwelling && word_reveal_timer.isActive()) {
                            word_reveal_timer.stop();
                        }

                        // Always emit a mode-transformed result (even if empty) so the full
                        // native ASR text can't leak through to the stream.
                        stream_output_result = build_word_reveal_display(
                                caption_result, interrupted, line_count, is_subtitle_box);
                        break;
                    }
                    break;
                }
                case STREAM_OUTPUT_MODE_LOW_LATENCY:
                default: {
                    const double delay = settings.format_settings.debounce_delay_seconds;
                    if (delay > 0 && !caption_result.final) {
                        // Buffer interims for `delay` seconds to let the ASR revise
                        if (!lowlatency_buffering) {
                            lowlatency_buffer_start = now;
                            lowlatency_buffering = true;
                        }
                        auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                                now - lowlatency_buffer_start).count();
                        if (elapsed < delay) {
                            // Still within delay window — suppress stream output, keep preview
                            to_stream = false;
                            to_recording = false;
                            break;
                        }
                        // Delay expired — show this result (latest revision)
                        lowlatency_buffering = false;
                    } else {
                        // Final or delay=0 — pass through, reset buffer
                        lowlatency_buffering = false;
                    }
                    break;
                }
            }
        }

        to_transcript_streaming = settings.transcript_settings.enabled && settings.transcript_settings.streaming_transcripts_enabled;
        to_transcript_recording = settings.transcript_settings.enabled && settings.transcript_settings.recording_transcripts_enabled;
        to_transcript_virtualcam = settings.transcript_settings.enabled && settings.transcript_settings.virtualcam_transcripts_enabled;

        if (settings.file_output_settings.isValidEnabled() && fileoutput_captions_output.control) {
            file_output_result = caption_result_handler->prepare_caption_output(
                caption_result,
                true,
                true,
                settings.file_output_settings.insert_punctuation,
                settings.file_output_settings.line_length,
                settings.file_output_settings.line_count,
                settings.file_output_settings.capitalization,
                interrupted,
                results_history);
        }
    }

    if (to_stream || to_recording) {
        this->output_caption_writers(
                CaptionOutput(stream_output_result ? stream_output_result : native_output_result, false),
                to_stream, to_recording,
                false, false, false,
                false);
    }
    if (to_transcript_streaming || to_transcript_recording || to_transcript_virtualcam) {
        // is_clearance=true on this call avoids a second caption_was_output() stamp;
        // the stream/recording call above already stamped it.
        this->output_caption_writers(
                CaptionOutput(native_output_result, false),
                false, false,
                to_transcript_streaming, to_transcript_recording, to_transcript_virtualcam,
                to_stream || to_recording);
    }

    if (file_output_result)
        fileoutput_captions_output.enqueue(CaptionOutput(file_output_result, false));

    for (const auto &text_out: text_source_sets) {
        set_text_source_text(std::get<0>(text_out), std::get<1>(text_out));
    }
    if (!text_source_sets.empty())
        caption_was_output();

    if (preview_this_result) {
        auto &preview_result = stream_output_result ? stream_output_result : native_output_result;
        emit caption_result_received(preview_result, false, recent_caption_text);
    }
}

void SourceCaptioner::output_caption_writers(
        const CaptionOutput &output,
        bool to_stream,
        bool to_recoding,
        bool to_transcript_streaming,
        bool to_transcript_recording,
        bool to_transcript_virtualcam,
        bool is_clearance) {

    bool sent_stream = false;
    if (to_stream) {
        sent_stream = streaming_output.enqueue(output);
    }

    bool sent_recording = false;
    if (to_recoding) {
        sent_recording = recording_output.enqueue(output);
    }

    if (sent_stream || sent_recording) {
        last_stream_caption_sent_at = std::chrono::steady_clock::now();
    }

    bool sent_transcript_streaming = false;
    if (to_transcript_streaming) {
        sent_transcript_streaming = transcript_streaming_output.enqueue(output);
    }

    bool sent_transcript_recording = false;
    if (to_transcript_recording) {
        sent_transcript_recording = transcript_recording_output.enqueue(output);
    }

    bool sent_transcript_virtualcam = false;
    if (to_transcript_virtualcam) {
        sent_transcript_virtualcam = transcript_virtualcam_output.enqueue(output);
    }

//    debug_log("queuing caption line , stream: %d, recording: %d, '%s'",
//              sent_stream, sent_recording, output.line.c_str());

    if (!is_clearance)
        caption_was_output();
}

void SourceCaptioner::caption_was_output() {
    this->last_caption_at = std::chrono::steady_clock::now();
    this->last_caption_cleared = false;
}


void SourceCaptioner::stream_started_event() {
    settings_change_mutex.lock();
    SourceCaptionerSettings cur_settings = settings;
    settings_change_mutex.unlock();

    auto control_output = std::make_shared<CaptionOutputControl<int>>(0);

    streaming_output.set_control(control_output);
    std::thread th(caption_output_writer_loop, control_output, true);
    th.detach();

    if (this->base_enabled && cur_settings.transcript_settings.enabled && cur_settings.transcript_settings.streaming_transcripts_enabled) {
        auto control_transcript = std::make_shared<CaptionOutputControl<TranscriptOutputSettings>>(cur_settings.transcript_settings);
        transcript_streaming_output.set_control(control_transcript);

        std::thread th2(transcript_writer_loop, control_transcript, "stream", cur_settings.transcript_settings);
        th2.detach();
    }
}

void SourceCaptioner::stream_stopped_event() {
    streaming_output.clear();
    transcript_streaming_output.clear();
}

void SourceCaptioner::recording_started_event() {
    settings_change_mutex.lock();
    SourceCaptionerSettings cur_settings = settings;
    settings_change_mutex.unlock();

    auto control_output = std::make_shared<CaptionOutputControl<int>>(0);

    recording_output.set_control(control_output);
    std::thread th(caption_output_writer_loop, control_output, false);
    th.detach();

    if (this->base_enabled && cur_settings.transcript_settings.enabled && cur_settings.transcript_settings.recording_transcripts_enabled) {
        auto control_transcript = std::make_shared<CaptionOutputControl<TranscriptOutputSettings>>(cur_settings.transcript_settings);
        transcript_recording_output.set_control(control_transcript);

        std::thread th2(transcript_writer_loop, control_transcript, "recording", cur_settings.transcript_settings);
        th2.detach();
    }
}

void SourceCaptioner::recording_stopped_event() {
    recording_output.clear();
    transcript_recording_output.clear();
}

void SourceCaptioner::virtualcam_started_event() {
    settings_change_mutex.lock();
    SourceCaptionerSettings cur_settings = settings;
    settings_change_mutex.unlock();

    if (this->base_enabled && cur_settings.transcript_settings.enabled && cur_settings.transcript_settings.virtualcam_transcripts_enabled) {
        auto control_transcript = std::make_shared<CaptionOutputControl<TranscriptOutputSettings>>(cur_settings.transcript_settings);
        transcript_virtualcam_output.set_control(control_transcript);

        std::thread th2(transcript_writer_loop, control_transcript, "virtualcam", cur_settings.transcript_settings);
        th2.detach();
    }
}

void SourceCaptioner::virtualcam_stopped_event() {
    transcript_virtualcam_output.clear();
}

void SourceCaptioner::set_text_source_text(const string &text_source_name, const string &caption_text) {
    if (std::get<0>(last_text_source_set) == caption_text && std::get<1>(last_text_source_set) == text_source_name) {
//        debug_log("ignore duplicate set_text_source_text");
        return;
    }

    ::set_text_source_text(text_source_name, caption_text);
    last_text_source_set = std::tuple<string, string>(caption_text, text_source_name);
}


SourceCaptioner::~SourceCaptioner() {
    stream_stopped_event();
    recording_stopped_event();
    stop_caption_stream(false);
}

template<class T>
void CaptionOutputControl<T>::stop_soon() {
    debug_log("CaptionOutputControl stop_soon()");
    stop = true;
    caption_queue.enqueue(CaptionOutput());
}

template<class T>
CaptionOutputControl<T>::~CaptionOutputControl() {
    debug_log("~CaptionOutputControl");
}

bool TranscriptOutputSettings::hasBaseSettings() const {
    if (!enabled || output_path.empty() || format.empty())
        return false;

    return true;
}
