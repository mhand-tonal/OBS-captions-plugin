#include "src/SourceCaptioner.h"
#include "src/stringutils.h"
#include "lib/caption_stream/log.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

// These entry points are defined in caption_transcript_writer.h, compiled by SourceCaptioner.cpp.
void transcript_writer_loop(shared_ptr<CaptionOutputControl<TranscriptOutputSettings>>, const string,
                            const TranscriptOutputSettings);
void fileoutput_writer_loop(shared_ptr<CaptionOutputControl<FileOutputSettings>>, const FileOutputSettings);

static vector<string> log_lines;

static void capture_log(int, const char *line) {
    log_lines.emplace_back(line);
}

static void require(bool condition, const string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

static string read_file(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot read " + path.toStdString());
    return file.readAll().toStdString();
}

static CaptionOutput caption(const string &text, bool final = true, int index = 0) {
    // Synthetic timestamps after the writer's start time keep these captions relevant.
    const auto start = std::chrono::steady_clock::now() + std::chrono::seconds(10 + index);
    CaptionResult result(index, final, 1.0, text, text, start, start + std::chrono::milliseconds(500));
    auto output = std::make_shared<OutputCaptionResult>(result, false);
    output->clean_caption_text = text;
    output->output_line = text;
    return CaptionOutput(output, false);
}

static TranscriptOutputSettings transcript_settings(const string &folder, const string &format) {
    return TranscriptOutputSettings(true, folder, format,
                                    "custom", "recording.txt", "overwrite",
                                    "custom", "transcript." + format, "overwrite",
                                    "custom", "virtualcam.txt", "overwrite",
                                    2, 0, false, false, CAPITALIZATION_NORMAL,
                                    true, true, false, false);
}

static void test_transcript_shutdown(const QString &folder) {
    for (const string format : {"txt_plain", "txt", "raw", "srt"}) {
        auto settings = transcript_settings(folder.toStdString(), format);
        auto control = std::make_shared<CaptionOutputControl<TranscriptOutputSettings>>(settings);
        control->caption_queue.enqueue(caption("first caption"));
        control->caption_queue.enqueue(caption("last caption", false, 1));
        control->stop_soon();
        transcript_writer_loop(control, "stream", settings);
        const auto contents = read_file(folder + "/transcript." + QString::fromStdString(format));
        const auto first = contents.find("first caption");
        const auto last = contents.find("last caption");
        require(first != string::npos && last != string::npos && first < last,
                format + " lost queued final/interim captions at shutdown");
        require(contents.find("first caption", first + 1) == string::npos,
                format + " duplicated a final caption");
    }
}

static void test_latest_file_shutdown(const QString &folder) {
    FileOutputSettings settings{};
    settings.enabled = true;
    settings.output_folder = folder.toStdString();
    settings.filename_type = "custom";
    settings.filename_custom = "latest.txt";
    settings.filename_exists = "overwrite";
    auto control = std::make_shared<CaptionOutputControl<FileOutputSettings>>(settings);
    control->caption_queue.enqueue(caption("old caption"));
    control->caption_queue.enqueue(caption("newest caption"));
    control->stop_soon();
    fileoutput_writer_loop(control, settings);
    require(read_file(folder + "/latest.txt") == "newest caption", "latest file lost the last queued caption");
}

static void test_failed_writers_stop(const QString &folder) {
    const auto missing = (folder + "/missing-directory").toStdString();
    auto settings = transcript_settings(missing, "txt_plain");
    auto transcript = std::make_shared<CaptionOutputControl<TranscriptOutputSettings>>(settings);
    transcript_writer_loop(transcript, "stream", settings);
    require(transcript->stop, "failed transcript writer still accepts captions");

    FileOutputSettings file_settings{};
    file_settings.output_folder = missing;
    auto file = std::make_shared<CaptionOutputControl<FileOutputSettings>>(file_settings);
    fileoutput_writer_loop(file, file_settings);
    require(file->stop, "failed latest-file writer still accepts captions");

    // The writer is in another translation unit; its errors must reach the shared sink.
    require(std::any_of(log_lines.begin(), log_lines.end(), [](const string &line) {
        return line.find("output dir not found") != string::npos;
    }), "writer errors did not reach the shared log sink");
}

static void test_utf8_caption_limit() {
    const string text = u8"one café 日本語 🎤 ending";
    require(utf8_tail_within_bytes("unchanged", 128) == "unchanged", "short caption changed");
    require(utf8_tail_within_bytes("abcdef", 3) == "def", "caption should retain its tail");
    for (size_t limit = 0; limit <= text.size() + 1; ++limit) {
        const string tail = utf8_tail_within_bytes(text, limit);
        require(tail.size() <= limit, "caption exceeds its byte limit");
        require(QString::fromUtf8(tail.data(), static_cast<int>(tail.size())).toUtf8().toStdString() == tail,
                "caption was cut inside a UTF-8 character");
    }
}

static void test_log_formatting() {
    const string long_message(9000, 'x');
    info_log("%s\nnext\rline", long_message.c_str());
    require(log_lines.back().find(long_message + " next line") != string::npos,
            "logger truncated or failed to flatten a long message");
    const auto count = log_lines.size();
    captions_log_level = CAPTIONS_LOG_ERROR;
    info_log("filtered");
    require(log_lines.size() == count, "logger ignored the level filter");
    captions_log_level = CAPTIONS_LOG_DEBUG;
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir folder;
    captions_log_sink = capture_log;
    try {
        require(folder.isValid(), "cannot create temporary test directory");
        test_transcript_shutdown(folder.path());
        test_latest_file_shutdown(folder.path());
        test_failed_writers_stop(folder.path());
        test_utf8_caption_limit();
        test_log_formatting();
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    std::cout << "Caption stability checks passed\n";
    return 0;
}
