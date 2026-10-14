// ccpkg — list, extract, rebuild and verify discimg.fse/discimg.pkg.

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <optional>

#include "psprecore/error.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "psprecore/text.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;

namespace {

constexpr nx::string_view kManifestName = "manifest.tsv";
constexpr nx::string_view kManifestHeader = "# ff7cc-pkg-manifest 1";

void usage() {
    std::fputs(
        "usage: ccpkg <command> [args]\n"
        "\n"
        "  info    <game>                    summary of the archive by file type\n"
        "  list    <game>                    one line per file: index, sector, size, type\n"
        "  extract <game> <out> [--range A-B]\n"
        "                                    write files to <out>/files and <out>/manifest.tsv\n"
        "  pack    <extracted> <out>         build <out>/discimg.fse + discimg.pkg from a manifest\n"
        "  verify  <game-a> <game-b>         compare two archives file by file\n"
        "\n"
        "<game> is a disc root, PSP_GAME, USRDIR, or a directory holding discimg.fse/pkg.\n"
        "Files in an extracted tree may be edited or resized before packing.\n",
        stderr);
}

struct Range {
    std::size_t first = 0;
    std::size_t last = 0;  // inclusive
};

Range parse_range(nx::string_view text, std::size_t count) {
    const auto dash = text.find('-');
    try {
        const std::size_t a = parse_u64(text.substr(0, dash));
        const std::size_t b = dash == nx::string_view::npos ? a : parse_u64(text.substr(dash + 1));
        if (a > b || b >= count) throw Error("");
        return Range{a, b};
    } catch (const std::exception&) {
        throw Error(nx::string("invalid --range '") + text + "' (archive has " + nx::format("{}", count) + " files)");
    }
}

nx::string file_name(std::size_t index, pkg::FileType type) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%05zu", index);
    return nx::string(buf) + pkg::type_extension(type);
}

nx::string human_size(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

void make_directories(nx::fs::path_view dir) {
    if (!nx::fs::create_directories(dir))
        throw IoError(nx::string("cannot create directory '") + dir.native() + "'");
}

// ---------------------------------------------------------------------------------------------

int cmd_info(nx::fs::path_view game) {
    const auto paths = pkg::locate(game);
    pkg::Reader reader(paths);

    struct Stat {
        std::size_t count = 0;
        std::uint64_t bytes = 0;
    };
    nx::map<pkg::FileType, Stat> stats;
    for (std::size_t i = 0; i < reader.size(); ++i) {
        auto& s = stats[reader.sniff(i)];
        ++s.count;
        s.bytes += reader.entry(i).size;
    }

    std::printf("index:   %s\narchive: %s (%s)\nfiles:   %zu\n\n", paths.fse.c_str(), paths.pkg.c_str(),
                human_size(reader.archive_size()).c_str(), reader.size());
    std::printf("%-8s %7s %12s\n", "type", "count", "size");
    for (const auto& [type, s] : stats)
        std::printf("%-8s %7zu %12s\n", nx::string(pkg::type_name(type)).c_str(), s.count,
                    human_size(s.bytes).c_str());
    return 0;
}

int cmd_list(nx::fs::path_view game) {
    pkg::Reader reader(pkg::locate(game));
    std::puts("index\tsector\tsize\ttype");
    for (std::size_t i = 0; i < reader.size(); ++i) {
        const auto& e = reader.entry(i);
        std::printf("%zu\t%u\t%u\t%s\n", i, e.sector, e.size, nx::string(pkg::type_name(reader.sniff(i))).c_str());
    }
    return 0;
}

int cmd_extract(nx::fs::path_view game, nx::fs::path_view out, std::optional<nx::string_view> range_text) {
    pkg::Reader reader(pkg::locate(game));
    const Range range = range_text ? parse_range(*range_text, reader.size()) : Range{0, reader.size() - 1};

    make_directories(path_join(out.native(), "files"));

    nx::string manifest(kManifestHeader);
    manifest += "\n# index\ttype\tsize\tpath\n";

    std::uint64_t total = 0;
    for (std::size_t i = range.first; i <= range.last; ++i) {
        const auto& e = reader.entry(i);
        const auto type = reader.sniff(i);
        nx::string rel = "-";
        if (e.size > 0) {
            rel = "files/" + file_name(i, type);
            io::File f(path_join(out.native(), rel), io::File::Mode::Write);
            reader.copy_to(i, f);
            f.close();
            total += e.size;
        }
        manifest += nx::format("{}\t{}\t{}\t{}\n", i, pkg::type_name(type), e.size, rel);
        if ((i - range.first + 1) % 1000 == 0)
            std::fprintf(stderr, "  %zu / %zu files\n", i - range.first + 1, range.last - range.first + 1);
    }
    if (!nx::fs::file_write_text(path_join(out.native(), kManifestName), manifest))
        throw IoError(nx::string("cannot write manifest in '") + out.native() + "'");
    std::printf("extracted %zu entries (%s) to %s\n", range.last - range.first + 1, human_size(total).c_str(),
                nx::string(out.native()).c_str());
    return 0;
}

int cmd_pack(nx::fs::path_view extracted, nx::fs::path_view out) {
    const auto manifest = nx::fs::file_read_text(path_join(extracted.native(), kManifestName));
    if (!manifest) throw IoError(nx::string("no ") + kManifestName + " in '" + extracted.native() + "'");

    struct Row {
        std::size_t index;
        nx::string path;
    };
    nx::vector<Row> rows;
    const nx::string_view text = manifest.value();
    std::size_t line_no = 0;
    bool header_seen = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == nx::string_view::npos ? text.size() : nl;
        nx::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line == kManifestHeader) header_seen = true;
        if (line.empty() || line.front() == '#') continue;
        // index \t type \t size \t path (the path is the rest of the line and must not be empty)
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == nx::string_view::npos ? t1 : line.find('\t', t1 + 1);
        const std::size_t t3 = t2 == nx::string_view::npos ? t2 : line.find('\t', t2 + 1);
        if (t3 == nx::string_view::npos || t3 + 1 == line.size())
            throw FormatError("manifest line " + nx::format("{}", line_no) + ": expected 4 tab-separated fields");
        rows.push_back(Row{parse_u64(line.substr(0, t1)), nx::string(line.substr(t3 + 1))});
    }
    if (!header_seen) throw FormatError(nx::string("manifest is missing the '") + kManifestHeader + "' header");

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].index != i)
            throw FormatError("manifest must list every index from 0 with no gaps (missing or duplicate " +
                              nx::format("{}", i) + "); extract the full archive before packing");

    make_directories(out);
    pkg::Writer writer(pkg::output_paths(out));
    for (const auto& row : rows) {
        if (row.path == "-")
            writer.add({});
        else
            writer.add_file(path_join(extracted.native(), row.path));
    }
    writer.finish();
    std::printf("packed %zu files into %s\n", writer.size(), nx::string(out.native()).c_str());
    return 0;
}

int cmd_verify(nx::fs::path_view a_dir, nx::fs::path_view b_dir) {
    pkg::Reader a(pkg::locate(a_dir));
    pkg::Reader b(pkg::locate(b_dir));
    if (a.size() != b.size()) {
        std::printf("file count differs: %zu vs %zu\n", a.size(), b.size());
        return 1;
    }

    constexpr std::size_t kChunk = 1 << 20;
    nx::vector<std::byte> buf_a(kChunk), buf_b(kChunk);
    std::size_t differences = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto size = a.entry(i).size;
        bool same = size == b.entry(i).size;
        for (std::uint64_t off = 0; same && off < size; off += kChunk) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - off));
            a.read_at(i, off, std::span(buf_a.data(), n));
            b.read_at(i, off, std::span(buf_b.data(), n));
            same = std::equal(buf_a.begin(), buf_a.begin() + n, buf_b.begin());
        }
        if (!same && ++differences <= 20)
            std::printf("file %zu differs (size %u vs %u)\n", i, size, b.entry(i).size);
    }
    if (differences == 0) {
        std::printf("all %zu files identical\n", a.size());
        return 0;
    }
    std::printf("%zu of %zu files differ\n", differences, a.size());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const nx::vector<nx::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 2 : 0;
    }
    try {
        const auto cmd = args[0];
        if (cmd == "info" && args.size() == 2) return cmd_info(args[1]);
        if (cmd == "list" && args.size() == 2) return cmd_list(args[1]);
        if (cmd == "extract" && (args.size() == 3 || (args.size() == 5 && args[3] == "--range")))
            return cmd_extract(args[1], args[2], args.size() == 5 ? std::optional(args[4]) : std::nullopt);
        if (cmd == "pack" && args.size() == 3) return cmd_pack(args[1], args[2]);
        if (cmd == "verify" && args.size() == 3) return cmd_verify(args[1], args[2]);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccpkg: %s\n", e.what());
        return 1;
    }
}
