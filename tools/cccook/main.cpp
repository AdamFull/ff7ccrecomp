// cccook — converts the game's ATRAC audio and PSMF movies into cooked media (see ff7cc/formats/cooked.hpp)
// using the FFmpeg command-line tool. FFmpeg is only needed here, never at runtime.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atrac.hpp"
#include "ff7cc/formats/cooked.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "ff7cc/formats/psmf.hpp"
#include "ff7cc/io/file.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

void usage() {
    std::fputs(
        "usage: cccook <game> <out> [options]\n"
        "\n"
        "Converts ATRAC3/ATRAC3plus audio to Ogg Vorbis and PSMF movies to AV1 (IVF) + Ogg Vorbis.\n"
        "Existing outputs are kept, so an interrupted run can be resumed.\n"
        "\n"
        "  --ffmpeg PATH     FFmpeg executable (default: ffmpeg on PATH)\n"
        "  --jobs N          parallel FFmpeg processes (default: half the CPU threads)\n"
        "  --only KIND       audio | movies\n"
        "  --limit N         cook at most N files of each kind (testing)\n"
        "  --entry N         cook only archive entry N (repeatable)\n"
        "  --force           overwrite existing outputs\n"
        "  --audio-quality Q Vorbis quality 0-10 (default 6)\n"
        "  --video-crf N     SVT-AV1 CRF (default 26)\n",
        stderr);
}

struct Options {
    std::string ffmpeg = "ffmpeg";
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    std::string only;
    std::size_t limit = static_cast<std::size_t>(-1);
    nx::vector<std::size_t> entries;
    bool force = false, keep_temp = false;
    int audio_quality = 6, video_crf = 26;
};

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

// Runs a command line through the shell; returns the exit status.
int run(const std::string& command) {
#ifdef _WIN32
    // cmd.exe strips the outer quotes of a command that starts with one; wrap the whole line once more.
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

void write_file(const fs::path& path, std::span<const std::byte> data) {
    io::File f(path, io::File::Mode::Write);
    f.write(data);
}

struct Task {
    std::string label;
    std::function<bool()> work;
};

class Cooker {
public:
    Cooker(const Options& options, fs::path out) : opt_(options), out_(std::move(out)), tmp_(out_ / "tmp") {
        fs::create_directories(out_ / "audio");
        fs::create_directories(out_ / "movies");
        fs::create_directories(tmp_);
    }

    std::string ffmpeg() const { return quote(opt_.ffmpeg) + " -hide_banner -loglevel error -nostdin -y"; }

    // Writes `data` to a temporary input, runs FFmpeg with `args` between "-i input" and the output, and moves
    // the result into place. Retries a few times: freshly written files can be briefly locked on Windows
    // (e.g. by antivirus scanning).
    bool transcode(const std::string& name, std::span<const std::byte> data, const std::string& input_args,
                   const std::string& output_args, const fs::path& out) {
        const fs::path in = tmp_ / name;
        const fs::path part = fs::path(out).concat(".part");
        std::error_code ec;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
            try {
                write_file(in, data);
            } catch (const std::exception&) {
                continue;
            }
            ok = run(ffmpeg() + " " + input_args + " -i " + quote(in) + " " + output_args + " " + quote(part)) == 0;
        }
        if (!opt_.keep_temp) fs::remove(in, ec);
        if (!ok) {
            fs::remove(part, ec);
            return false;
        }
        for (int attempt = 0; attempt < 5; ++attempt) {
            fs::rename(part, out, ec);
            if (!ec) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    std::string vorbis_args() const {
        // Regenerate timestamps: FFmpeg's ATRAC3plus decoder repeats them, which corrupts Ogg granule positions.
        return "-af asetpts=N/SR/TB -c:a libvorbis -q:a " + std::to_string(opt_.audio_quality) + " -f ogg";
    }

    // RIFF ATRAC -> Ogg Vorbis (the raw decoder timeline; the runtime applies delay and loops itself).
    bool cook_audio(std::size_t entry, const nx::vector<std::byte>& file, std::uint64_t key) {
        return transcode("a" + std::to_string(entry) + ".at3", file, "", vorbis_args(), cooked::audio_path(out_, key));
    }

    // PSMF -> AV1 video (one frame per access unit, in display order) + Ogg Vorbis audio.
    bool cook_movie(std::size_t entry, const nx::vector<std::byte>& file, std::uint64_t key) {
        psmf::Header header;
        if (!psmf::parse_header(file, header)) return false;
        psmf::Demuxer demux;
        nx::vector<std::byte> video, audio;
        std::uint64_t index = 0;
        const auto drain = [&] {
            for (auto& u : demux.units(psmf::kVideo)) {
                const auto* b = reinterpret_cast<const std::byte*>(u.data.data());
                video.insert(video.end(), b, b + u.data.size());
            }
            demux.units(psmf::kVideo).clear();
            for (auto& u : demux.units(psmf::kAudio)) {
                if (u.data.size() <= 8) continue;
                const auto* b = reinterpret_cast<const std::byte*>(u.data.data());
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

        const std::string stem = "m" + std::to_string(entry);
        bool ok = true;
        const fs::path video_out = cooked::movie_video_path(out_, key);
        if (!video.empty() && (opt_.force || !fs::exists(video_out)))
            ok = transcode(stem + ".h264", video, "-framerate 30000/1001 -f h264",
                           "-c:v libsvtav1 -preset 6 -crf " + std::to_string(opt_.video_crf) +
                               " -g 300 -pix_fmt yuv420p -f ivf",
                           video_out);
        const fs::path audio_out = cooked::movie_audio_path(out_, key);
        if (ok && !audio.empty() && demux.atrac_params() && (opt_.force || !fs::exists(audio_out)))
            ok = transcode(stem + ".at3", atrac::make_atrac3plus_riff(audio, demux.atrac_params(), 2), "",
                           vorbis_args(), audio_out);
        return ok;
    }

private:
    Options opt_;
    fs::path out_, tmp_;
};

int cook(const fs::path& game, const fs::path& out, const Options& opt) {
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
        static constexpr std::byte kRiff[] = {std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}};
        for (auto it = file.begin(); (it = std::search(it, file.end(), std::begin(kRiff), std::end(kRiff))) != file.end(); ++it) {
            const std::size_t at = static_cast<std::size_t>(it - file.begin());
            atrac::Info info;
            if (!atrac::parse(std::span(file).subspan(at), info)) continue;
            const std::size_t length = info.data_offset + info.data_size;
            const std::uint64_t key = cooked::fnv1a64(std::span(file).subspan(at, length));
            if (!seen.emplace(key, i).second) {
                ++duplicates;
                continue;
            }
            if (fs::exists(cooked::audio_path(out, key)) && !opt.force) {
                ++skipped;
                continue;
            }
            ++audio_count;
            ++embedded;
            nx::vector<std::byte> clip(it, it + static_cast<std::ptrdiff_t>(length));
            tasks.push_back({"audio " + std::to_string(i) + "+" + std::to_string(at),
                             [&cooker, i, key, clip = std::move(clip)] { return cooker.cook_audio(i, clip, key); }});
        }
    };

    nx::vector<std::byte> head(2048);
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
            key = cooked::fnv1a64(std::span(head).first(n));
        } else {
            auto file = reader.read(i);
            atrac::Info info;
            if (!atrac::parse(file, info)) continue;
            key = cooked::fnv1a64(std::span(file).first(info.data_offset + info.data_size));
        }
        if (const auto it = seen.find(key); it != seen.end()) {
            ++duplicates;
            continue;
        }
        seen.emplace(key, i);
        const bool exists = is_audio ? fs::exists(cooked::audio_path(out, key))
                                     : fs::exists(cooked::movie_video_path(out, key)) &&
                                           fs::exists(cooked::movie_audio_path(out, key));
        if (exists && !opt.force) {
            ++skipped;
            continue;
        }
        (is_audio ? audio_count : movie_count)++;
        tasks.push_back({(is_audio ? "audio " : "movie ") + std::to_string(i), [&reader, &cooker, i, key, is_audio] {
                             nx::vector<std::byte> file;
                             {
                                 static std::mutex m;  // one pkg file handle
                                 std::lock_guard lock(m);
                                 file = reader.read(i);
                             }
                             return is_audio ? cooker.cook_audio(i, file, key) : cooker.cook_movie(i, file, key);
                         }});
    }
    // Movies first: they take longest, so the pool stays busy.
    std::stable_partition(tasks.begin(), tasks.end(), [](const Task& t) { return t.label.starts_with("movie"); });
    std::fprintf(stderr,
                 "cooking %zu audio files (%zu embedded in other entries) and %zu movies with %u jobs "
                 "(%zu already cooked, %zu duplicates)\n",
                 audio_count, embedded, movie_count, opt.jobs, skipped, duplicates);

    std::atomic<std::size_t> next{0}, done{0}, failed{0};
    std::mutex log;
    nx::vector<std::thread> pool;
    for (unsigned j = 0; j < opt.jobs; ++j)
        pool.emplace_back([&] {
            for (std::size_t t; (t = next++) < tasks.size();) {
                bool ok = false;
                try {
                    ok = tasks[t].work();
                } catch (const std::exception& e) {
                    std::lock_guard lock(log);
                    std::fprintf(stderr, "%s: %s\n", tasks[t].label.c_str(), e.what());
                }
                const std::size_t n = ++done;
                if (!ok) ++failed;
                std::lock_guard lock(log);
                std::fprintf(stderr, "[%zu/%zu] %s%s\n", n, tasks.size(), tasks[t].label.c_str(), ok ? "" : "  FAILED");
            }
        });
    for (auto& t : pool) t.join();
    std::error_code ec;
    if (!opt.keep_temp) fs::remove(out / "tmp", ec);
    std::fprintf(stderr, "done: %zu cooked, %zu failed\n", tasks.size() - failed.load(), failed.load());
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    try {
        Options opt;
        for (int i = 3; i < argc; ++i) {
            const std::string_view a = argv[i];
            const auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw Error("missing value for " + std::string(a));
                return argv[++i];
            };
            if (a == "--ffmpeg") opt.ffmpeg = next();
            else if (a == "--jobs") opt.jobs = std::max(1u, static_cast<unsigned>(std::stoul(next())));
            else if (a == "--only") opt.only = next();
            else if (a == "--limit") opt.limit = std::stoull(next());
            else if (a == "--entry") opt.entries.push_back(std::stoull(next()));
            else if (a == "--force") opt.force = true;
            else if (a == "--keep-temp") opt.keep_temp = true;
            else if (a == "--audio-quality") opt.audio_quality = std::stoi(next());
            else if (a == "--video-crf") opt.video_crf = std::stoi(next());
            else throw Error("unknown option " + std::string(a));
        }
        return cook(argv[1], argv[2], opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cccook: %s\n", e.what());
        return 1;
    }
}
