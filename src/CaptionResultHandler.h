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

#ifndef CPPTESTING_CAPTIONRESULTHANDLER_H
#define CPPTESTING_CAPTIONRESULTHANDLER_H

#include <string>
#include <CaptionStream.h>
#include "WordReplacer.h"

enum CapitalizationType {
    CAPITALIZATION_NORMAL = 0,
    CAPITALIZATION_ALL_CAPS = 1,
    CAPITALIZATION_ALL_LOWERCASE = 2,
};

enum StreamCaptionOutputMode {
    STREAM_OUTPUT_MODE_LOW_LATENCY = 0,
    STREAM_OUTPUT_MODE_DEBOUNCED = 1,
    STREAM_OUTPUT_MODE_APPEND_ONLY = 2,
    STREAM_OUTPUT_MODE_GROWING = 3,
    STREAM_OUTPUT_MODE_SUBTITLE_BOX = 4,
};

static constexpr double DEFAULT_DEBOUNCE_SECONDS = 0.5;
static constexpr double MIN_DEBOUNCE_SECONDS = 0.1;
static constexpr double MAX_DEBOUNCE_SECONDS = 5.0;
static constexpr double DEFAULT_STABILITY_THRESHOLD = 0.5;
static constexpr int DEFAULT_WORD_REVEAL_DELAY_MS = 0;
static constexpr int MIN_WORD_REVEAL_DELAY_MS = 0;
static constexpr int MAX_WORD_REVEAL_DELAY_MS = 2000;
static constexpr double DEFAULT_CARD_DWELL_SECONDS = 1.0;
static constexpr double MIN_CARD_DWELL_SECONDS = 0.0;
static constexpr double MAX_CARD_DWELL_SECONDS = 3.0;


class DefaultReplacer {
private:
    Replacer text_replacer;
    std::vector<string> default_replacements;
    std::vector<WordReplacement> manual_replacements;

public:
    const std::vector<WordReplacement> &user_replacements() const {
        return manual_replacements;
    }

    bool has_replacements() const {
        return text_replacer.has_replacements();
    }

    DefaultReplacer(const vector<string> &defaultReplacements,
                    const vector<WordReplacement> &manualReplacements
    ) : default_replacements(defaultReplacements),
        manual_replacements(manualReplacements),
        text_replacer(Replacer(combineWordReps(wordRepsFromStrs("regex_case_insensitive", defaultReplacements),
                                               manualReplacements), true)) {}

    bool operator==(const DefaultReplacer &rhs) const {
        return manual_replacements == rhs.manual_replacements &&
               default_replacements == rhs.default_replacements;
    }

    bool operator!=(const DefaultReplacer &rhs) const {
        return !(rhs == *this);
    }

    const Replacer &get_replacer() const {
        return text_replacer;
    }
};

struct CaptionFormatSettings {
    uint caption_line_length;
    uint caption_line_count;
    CapitalizationType capitalization;
    bool caption_insert_newlines;
    bool caption_insert_punctuation;

    bool caption_timeout_enabled;
    double caption_timeout_seconds;
    StreamCaptionOutputMode stream_caption_output_mode;
    double debounce_delay_seconds;
    double append_stability_threshold;
    int word_reveal_delay_ms;
    double card_dwell_seconds;
    DefaultReplacer replacer;

    CaptionFormatSettings(
            uint caption_line_length,
            uint caption_line_count,
            CapitalizationType capitalization,
            bool caption_insert_newlines,
            bool caption_insert_punctuation,
            const DefaultReplacer &replacer,
            bool caption_timeout_enabled,
            double caption_timeout_seconds,
            StreamCaptionOutputMode stream_caption_output_mode = STREAM_OUTPUT_MODE_LOW_LATENCY,
            double debounce_delay_seconds = DEFAULT_DEBOUNCE_SECONDS,
            double append_stability_threshold = DEFAULT_STABILITY_THRESHOLD,
            int word_reveal_delay_ms = DEFAULT_WORD_REVEAL_DELAY_MS,
            double card_dwell_seconds = DEFAULT_CARD_DWELL_SECONDS
    ) :
            caption_line_length(caption_line_length),
            caption_line_count(caption_line_count),
            capitalization(capitalization),
            caption_insert_newlines(caption_insert_newlines),
            caption_insert_punctuation(caption_insert_punctuation),
            replacer(replacer),
            caption_timeout_enabled(caption_timeout_enabled),
            caption_timeout_seconds(caption_timeout_seconds),
            stream_caption_output_mode(stream_caption_output_mode),
            debounce_delay_seconds(debounce_delay_seconds),
            append_stability_threshold(append_stability_threshold),
            word_reveal_delay_ms(word_reveal_delay_ms),
            card_dwell_seconds(card_dwell_seconds) {
    }

    void print(const char *line_prefix = "") {
        printf("%sCaptionFormatSettings\n", line_prefix);
        printf("%s  caption_line_length: %d\n", line_prefix, caption_line_length);
        printf("%s  caption_line_count: %d\n", line_prefix, caption_line_count);
        printf("%s  capitalization: %d\n", line_prefix, capitalization);
        printf("%s  caption_insert_newlines: %d\n", line_prefix, caption_insert_newlines);
        printf("%s  caption_insert_punctuation: %d\n", line_prefix, caption_insert_punctuation);
        printf("%s  stream_caption_output_mode: %d\n", line_prefix, stream_caption_output_mode);
        printf("%s  debounce_delay_seconds: %f\n", line_prefix, debounce_delay_seconds);
        printf("%s  append_stability_threshold: %f\n", line_prefix, append_stability_threshold);
        printf("%s  word_reveal_delay_ms: %d\n", line_prefix, word_reveal_delay_ms);
        printf("%s  card_dwell_seconds: %f\n", line_prefix, card_dwell_seconds);
        printf("%s  user_replacements: %lu\n", line_prefix, replacer.user_replacements().size());
        for (auto &word : replacer.user_replacements())
            printf("%s        %s '%s' -> '%s'\n",
                   line_prefix, word.get_type().c_str(), word.get_from().c_str(), word.get_to().c_str());

//        printf("%s-----------\n", line_prefix);
    }

    bool operator==(const CaptionFormatSettings &rhs) const {
        return caption_line_length == rhs.caption_line_length &&
               caption_line_count == rhs.caption_line_count &&
               capitalization == rhs.capitalization &&
               caption_insert_newlines == rhs.caption_insert_newlines &&
               caption_insert_punctuation == rhs.caption_insert_punctuation &&
               replacer == rhs.replacer &&
               caption_timeout_enabled == rhs.caption_timeout_enabled &&
               caption_timeout_seconds == rhs.caption_timeout_seconds &&
               stream_caption_output_mode == rhs.stream_caption_output_mode &&
               debounce_delay_seconds == rhs.debounce_delay_seconds &&
               word_reveal_delay_ms == rhs.word_reveal_delay_ms &&
               card_dwell_seconds == rhs.card_dwell_seconds &&
               append_stability_threshold == rhs.append_stability_threshold;
    }

    bool operator!=(const CaptionFormatSettings &rhs) const {
        return !(rhs == *this);
    }


};


struct OutputCaptionResult {
    CaptionResult caption_result;
    bool interrupted;
    string clean_caption_text;

    vector<string> output_lines; //optionally cleaned and filled with history
    string output_line; // joined output_lines

    explicit OutputCaptionResult(
            const CaptionResult &caption_result,
            const bool interrupted
    ) : caption_result(caption_result), interrupted(interrupted) {}
};


class CaptionResultHandler {
    CaptionFormatSettings settings;

//    std::shared_ptr<CaptionResult> last_final_result;
//    string last_line;

public:
    explicit CaptionResultHandler(CaptionFormatSettings settings);

    shared_ptr<OutputCaptionResult> prepare_caption_output(
            const CaptionResult &caption_result,
            const bool fillup_with_previous,
            const bool insert_newlines,
            const bool punctuation,
            const uint line_length,
            const uint targeted_line_count,
            const CapitalizationType capitalization,
            const bool interrupted,
            const std::vector<std::shared_ptr<OutputCaptionResult>> &result_history);

};

#endif //CPPTESTING_CAPTIONRESULTHANDLER_H
