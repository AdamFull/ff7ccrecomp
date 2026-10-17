// cccook — converts the game's ATRAC audio and PSMF movies into cooked media (see ff7cc/formats/cooked.hpp)
// using the FFmpeg command-line tool. FFmpeg is only needed here, never at runtime.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "psprecore/error.hpp"
#include "psprecore/cli.hpp"
#include "psprecore/formats/atrac.hpp"
#include "psprecore/formats/cooked.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "psprecore/formats/psmf.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "psprecore/text.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;

namespace {

struct Options {
    nx::string ffmpeg = "ffmpeg";
    unsigned jobs = std::max(1u, nx::cpu::logical_core_count() / 2);
    nx::string only;
    std::size_t limit = static_cast<std::size_t>(-1);
    nx::vector<std::size_t> entries;
    bool force = false, keep_temp = false;
    int audio_quality = 6, video_crf = 26;
};

nx::string quote(nx::fs::path_view p) { return nx::string("\"") + p.native() + "\""; }

// Runs a command line through the shell; returns the exit status.
int run(const nx::string& command) {
#ifdef _WIN32
    // cmd.exe strips the outer quotes of a command that starts with one; wrap the whole line once more.
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

void write_file(nx::fs::path_view path, std::span<const u8> data) {
    io::File f(path, io::File::Mode::Write);
    f.write(data);
}

struct Task {
    nx::string label;
    nx::unique_function<bool()> work;
};

class Cooker {
public:
    Cooker(const Options& options, nx::fs::path_view out)
        : opt_(options), out_(out.native()), tmp_(path_join(out_, "tmp")) {
        for (const nx::string& dir : {path_join(out_, "audio"), path_join(out_, "movies"), tmp_})
            if (!nx::fs::create_directories(dir)) throw Exception("cannot create directory '" + dir + "'");
    }

    nx::string ffmpeg() const { return quote(opt_.ffmpeg) + " -hide_banner -loglevel error -nostdin -y"; }

    // Writes `data` to a temporary input, runs FFmpeg with `args` between "-i input" and the output, and moves
    // the result into place. Retries a few times: freshly written files can be briefly locked on Windows
    // (e.g. by antivirus scanning).
    bool transcode(nx::string_view name, std::span<const u8> data, nx::string_view input_args,
                   nx::string_view output_args, nx::fs::path_view out) {
        const nx::string in = path_join(tmp_, name);
        const nx::string part = nx::string(out.native()) + ".part";
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt) nx::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
            try {
                write_file(in, data);
            } catch (const std::exception&) {
                continue;
            }
            ok = run(ffmpeg() + " " + input_args + " -i " + quote(in) + " " + output_args + " " + quote(part)) == 0;
        }
        if (!opt_.keep_temp) (void)nx::fs::remove(in);
        if (!ok) {
            (void)nx::fs::remove(part);
            return false;
        }
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (nx::fs::rename(part, out)) return true;
            nx::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    nx::string vorbis_args() const {
        // Regenerate timestamps: FFmpeg's ATRAC3plus decoder repeats them, which corrupts Ogg granule positions.
        return nx::format("-af asetpts=N/SR/TB -c:a libvorbis -q:a {} -f ogg", opt_.audio_quality);
    }

    // RIFF ATRAC -> Ogg Vorbis (the raw decoder timeline; the runtime applies delay and loops itself).
    bool cook_audio(std::size_t entry, const nx::vector<u8>& file, std::uint64_t key) {
        return transcode(nx::format("a{}.at3", entry), file, "", vorbis_args(), cooked::audio_path(out_, key));
    }

    // PSMF -> AV1 video (one frame per access unit, in display order) + Ogg Vorbis audio.
    bool cook_movie(std::size_t entry, const nx::vector<u8>& file, std::uint64_t key) {
        const auto parsed = psmf::parse_header(file);
        if (!parsed) return false;
        const psmf::Header& header = *parsed;
        psmf::Demuxer demux;
        nx::vector<u8> video, audio;
        std::uint64_t index = 0;
        const auto drain = [&] {
            for (auto& u : demux.units(psmf::kVideo)) {
                const auto* b = reinterpret_cast<const u8*>(u.data.data());
                video.insert(video.end(), b, b + u.data.size());
            }
            demux.units(psmf::kVideo).clear();
            for (auto& u : demux.units(psmf::kAudio)) {
                if (u.data.size() <= 8) continue;
                const auto* b = reinterpret_cast<const u8*>(u.data.data());
                audio.insert(audio.end(), b + 8, b + u.data.size());  // strip the 8-byte frame header
            }
            demux.units(psmf::kAudio).clear();
        };
        for (std::size_t pos = header.data_offset; pos + psmf::kPackSize <= file.size(); pos += psmf::kPackSize) {
            demux.feed(std::span(file).subspan(pos, psmf::kPackSize), index++);
            drain();
        }
        demux.finish();
        drain();

        const nx::string stem = nx::format("m{}", entry);
        bool ok = true;
        const nx::string video_out = cooked::movie_video_path(out_, key);
        if (!video.empty() && (opt_.force || !nx::fs::exists(video_out)))
            ok = transcode(stem + ".h264", video, "-framerate 30000/1001 -f h264",
                           nx::format("-c:v libsvtav1 -preset 6 -crf {} -g 300 -pix_fmt yuv420p -f ivf", opt_.video_crf),
                           video_out);
        const nx::string audio_out = cooked::movie_audio_path(out_, key);
        if (ok && !audio.empty() && demux.atrac_params() && (opt_.force || !nx::fs::exists(audio_out)))
            ok = transcode(stem + ".at3", atrac::make_atrac3plus_riff(audio, demux.atrac_params(), 2), "",
                           vorbis_args(), audio_out);
        return ok;
    }

private:
    Options opt_;
    nx::string out_, tmp_;
};

int cook(nx::fs::path_view game, nx::fs::path_view out_dir, const Options& opt) {
    const nx::string_view out = out_dir.native();
#ifdef _WIN32
    _putenv_s("SVT_LOG", "1");  // SVT-AV1: errors only
#else
    setenv("SVT_LOG", "1", 1);
#endif
    pkg::Reader reader(pkg::locate(game));
    Cooker cooker(opt, out);
    nx::vector<Task> tasks;
    nx::map<std::uint64_t, std::size_t> seen;  // key -> first entry
    std::size_t audio_count = 0, movie_count = 0, skipped = 0, duplicates = 0;

    // RIFF ATRAC files stored inside other entries (e.g. containers the game hands to sceAtrac in place).
    // Each gets its own task holding a copy of the embedded bytes.
    std::size_t embedded = 0;
    const auto scan_embedded = [&](std::size_t i) {
        const auto file = reader.read(i);
        static constexpr u8 kRiff[] = {u8{'R'}, u8{'I'}, u8{'F'}, u8{'F'}};
        for (auto it = file.begin(); (it = std::search(it, file.end(), std::begin(kRiff), std::end(kRiff))) != file.end(); ++it) {
            const std::size_t at = static_cast<std::size_t>(it - file.begin());
            const auto parsed = atrac::parse(std::span(file).subspan(at));
            if (!parsed) continue;
            const atrac::Info& info = *parsed;
            const std::size_t length = info.data_offset + info.data_size;
            const std::uint64_t key = cooked::key_of(std::span(file).subspan(at, length));
            if (!seen.emplace(key, i).second) {
                ++duplicates;
                continue;
            }
            if (nx::fs::exists(cooked::audio_path(out, key)) && !opt.force) {
                ++skipped;
                continue;
            }
            ++audio_count;
            ++embedded;
            nx::vector<u8> clip(it, it + static_cast<std::ptrdiff_t>(length));
            tasks.push_back({nx::format("audio {}+{}", i, at),
                             nx::unique_function<bool()>([&cooker, i, key, clip = std::move(clip)] {
                                 return cooker.cook_audio(i, clip, key);
                             })});
        }
    };

    nx::vector<u8> head(2048);
    for (std::size_t i = 0; i < reader.size(); ++i) {
        if (!opt.entries.empty() && std::find(opt.entries.begin(), opt.entries.end(), i) == opt.entries.end()) continue;
        const pkg::FileType type = reader.sniff(i);
        const bool is_audio = type == pkg::FileType::Riff, is_movie = type == pkg::FileType::Psmf;
        if (!is_audio && !is_movie) {
            if (opt.only != "movies" && audio_count < opt.limit && type != pkg::FileType::Empty) scan_embedded(i);
            continue;
        }
        if ((is_audio && opt.only == "movies") || (is_movie && opt.only == "audio")) continue;
        if ((is_audio ? audio_count : movie_count) >= opt.limit) continue;

        std::uint64_t key = 0;
        if (is_movie) {
            const std::size_t n = reader.read_head(i, head);
            key = cooked::key_of(std::span(head).first(n));
        } else {
            auto file = reader.read(i);
            const auto parsed = atrac::parse(file);
            if (!parsed) continue;
            const atrac::Info& info = *parsed;
            key = cooked::key_of(std::span(file).first(info.data_offset + info.data_size));
        }
        if (const auto it = seen.find(key); it != seen.end()) {
            ++duplicates;
            continue;
        }
        seen.emplace(key, i);
        const bool exists = is_audio ? nx::fs::exists(cooked::audio_path(out, key))
                                     : nx::fs::exists(cooked::movie_video_path(out, key)) &&
                                           nx::fs::exists(cooked::movie_audio_path(out, key));
        if (exists && !opt.force) {
            ++skipped;
            continue;
        }
        (is_audio ? audio_count : movie_count)++;
        tasks.push_back({nx::format("{} {}", is_audio ? "audio" : "movie", i),
                         nx::unique_function<bool()>([&reader, &cooker, i, key, is_audio] {
                             nx::vector<u8> file;
                             {
                                 static nx::mutex m;  // one pkg file handle
                                 nx::scoped_lock lock(m);
                                 file = reader.read(i);
                             }
                             return is_audio ? cooker.cook_audio(i, file, key) : cooker.cook_movie(i, file, key);
                         })});
    }
    // Movies first: they take longest, so the pool stays busy.
    std::stable_partition(tasks.begin(), tasks.end(), [](const Task& t) { return t.label.starts_with("movie"); });
    std::fprintf(stderr,
                 "cooking %zu audio files (%zu embedded in other entries) and %zu movies with %u jobs "
                 "(%zu already cooked, %zu duplicates)\n",
                 audio_count, embedded, movie_count, opt.jobs, skipped, duplicates);

    std::atomic<std::size_t> next{0}, done{0}, failed{0};
    nx::mutex log;
    nx::vector<nx::thread> pool;
    for (unsigned j = 0; j < opt.jobs; ++j)
        pool.emplace_back([&] {
            for (std::size_t t; (t = next++) < tasks.size();) {
                bool ok = false;
                try {
                    ok = tasks[t].work();
                } catch (const std::exception& e) {
                    nx::scoped_lock lock(log);
                    std::fprintf(stderr, "%s: %s\n", tasks[t].label.c_str(), e.what());
                }
                const std::size_t n = ++done;
                if (!ok) ++failed;
                nx::scoped_lock lock(log);
                std::fprintf(stderr, "[%zu/%zu] %s%s\n", n, tasks.size(), tasks[t].label.c_str(), ok ? "" : "  FAILED");
            }
        });
    for (auto& t : pool) t.join();
    if (!opt.keep_temp) (void)nx::fs::remove(path_join(out, "tmp"));
    std::fprintf(stderr, "done: %zu cooked, %zu failed\n", tasks.size() - failed.load(), failed.load());
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    nx::string game, out;
    nx::cli::Parser cli("cccook", "Converts ATRAC3/ATRAC3plus audio to Ogg Vorbis and PSMF movies to AV1 (IVF) + Ogg "
                                  "Vorbis. Existing outputs are kept, so an interrupted run can be resumed.");
    cli.root()
        .positional("game", game, "the game (extracted disc folder)")
        .positional("out", out, "cooked media folder")
        .option("ffmpeg", opt.ffmpeg, "FFmpeg executable (default: ffmpeg on PATH)", {.value_name = "PATH"})
        .option("jobs", opt.jobs, "parallel FFmpeg processes (default: half the CPU threads)", {.value_name = "N"})
        .option("only", opt.only, "cook one kind only", {.value_name = "KIND", .choices = {"audio", "movies"}})
        .option("limit", opt.limit, "cook at most N files of each kind (testing)", {.value_name = "N"})
        .option_many("entry", opt.entries, "cook only archive entry N (repeatable)", {.value_name = "N"})
        .flag("force", opt.force, "overwrite existing outputs")
        .flag("keep-temp", opt.keep_temp, "keep FFmpeg's intermediate files")
        .option("audio-quality", opt.audio_quality, "Vorbis quality 0-10 (default: 6)", {.value_name = "Q"})
        .option("video-crf", opt.video_crf, "SVT-AV1 CRF (default: 26)", {.value_name = "N"});
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;
    try {
        opt.jobs = std::max(1u, opt.jobs);
        return cook(game, out, opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cccook: %s\n", e.what());
        return 1;
    }
}
