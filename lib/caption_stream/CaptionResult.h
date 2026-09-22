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
#ifndef OBS_GOOGLE_CAPTION_PLUGIN_CAPTIONRESULT_H
#define OBS_GOOGLE_CAPTION_PLUGIN_CAPTIONRESULT_H

#include <vector>

using namespace std;

struct CaptionWord {
    string word;
    double start = 0.0; // seconds from stream start
    double end = 0.0;

    CaptionWord() {}
    CaptionWord(string word, double start, double end)
        : word(std::move(word)), start(start), end(end) {}
};

struct CaptionResult {

    int index = 0;
    bool final = false;
    bool speech_final = false;
    double stability = 0.0;
    string caption_text;
    string raw_message;

    std::chrono::steady_clock::time_point first_received_at;
    std::chrono::steady_clock::time_point received_at;

    std::vector<CaptionWord> words; // word-level timestamps (populated by backends that support them)

    CaptionResult() {};

    CaptionResult(
            int index,
            bool final,
            double stability,
            string caption_text,
            string raw_message,
            std::chrono::steady_clock::time_point first_received_at,
            std::chrono::steady_clock::time_point received_at
    ) :
            index(index),
            final(final),
            stability(stability),
            caption_text(caption_text),
            raw_message(raw_message),
            first_received_at(first_received_at),
            received_at(received_at) {
    }
};


#endif //OBS_GOOGLE_CAPTION_PLUGIN_CAPTIONRESULT_H
