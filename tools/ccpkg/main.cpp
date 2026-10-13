// ccpkg — list, extract, rebuild and verify discimg.fse/discimg.pkg.

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "ff7cc/io/file.hpp"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

constexpr std::string_view kManifestName = "manifest.tsv";
constexpr std::string_view kManifestHeader = "# ff7cc-pkg-manifest 1";

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

Range parse_range(std::string_view text, std::size_t count) {
    const auto dash = text.find('-');
    try {
        const std::size_t a = std::stoul(std::string(text.substr(0, dash)));
        const std::size_t b = dash == std::string_view::npos ? a : std::stoul(std::string(text.substr(dash + 1)));
        if (a > b || b >= count) throw Error("");
        return Range{a, b};
    } catch (const std::exception&) {
        throw Error("invalid --range '" + std::string(text) + "' (archive has " + std::to_string(count) + " files)");
    }
}

std::string file_name(std::size_t index, pkg::FileType type) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%05zu", index);
    return std::string(buf) + std::string(pkg::type_extension(type));
}

std::string human_size(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

// ---------------------------------------------------------------------------------------------

int cmd_info(const fs::path& game) {
    const auto paths = pkg::locate(game);
    pkg::Reader reader(paths);

    struct Stat {
        std::size_t count = 0;
        std::uint64_t bytes = 0;
    };
    std::map<pkg::FileType, Stat> stats;
    for (std::size_t i = 0; i < reader.size(); ++i) {
        auto& s = stats[reader.sniff(i)];
        ++s.count;
        s.bytes += reader.entry(i).size;
    }

    std::printf("index:   %s\narchive: %s (%s)\nfiles:   %zu\n\n", paths.fse.string().c_str(),
                paths.pkg.string().c_str(), human_size(reader.archive_size()).c_str(), reader.size());
    std::printf("%-8s %7s %12s\n", "type", "count", "size");
    for (const auto& [type, s] : stats)
        std::printf("%-8s %7zu %12s\n", std::string(pkg::type_name(type)).c_str(), s.count,
                    human_size(s.bytes).c_str());
    return 0;
}

int cmd_list(const fs::path& game) {
    pkg::Reader reader(pkg::locate(game));
    std::puts("index\tsector\tsize\ttype");
    for (std::size_t i = 0; i < reader.size(); ++i) {
        const auto& e = reader.entry(i);
        std::printf("%zu\t%u\t%u\t%s\n", i, e.sector, e.size, std::string(pkg::type_name(reader.sniff(i))).c_str());
    }
    return 0;
}

int cmd_extract(const fs::path& game, const fs::path& out, std::optional<std::string_view> range_text) {
    pkg::Reader reader(pkg::locate(game));
    const Range range = range_text ? parse_range(*range_text, reader.size()) : Range{0, reader.size() - 1};

    const fs::path files_dir = out / "files";
    fs::create_directories(files_dir);

    std::ofstream manifest(out / kManifestName, std::ios::binary);
    if (!manifest) throw IoError("cannot write manifest in '" + out.string() + "'");
    manifest << kManifestHeader << "\n# index\ttype\tsize\tpath\n";

    std::uint64_t total = 0;
    for (std::size_t i = range.first; i <= range.last; ++i) {
        const auto& e = reader.entry(i);
        const auto type = reader.sniff(i);
        std::string rel = "-";
        if (e.size > 0) {
            rel = "files/" + file_name(i, type);
            io::File f(out / rel, io::File::Mode::Write);
            reader.copy_to(i, f);
            f.close();
            total += e.size;
        }
        manifest << i << '\t' << pkg::type_name(type) << '\t' << e.size << '\t' << rel << '\n';
        if ((i - range.first + 1) % 1000 == 0)
            std::fprintf(stderr, "  %zu / %zu files\n", i - range.first + 1, range.last - range.first + 1);
    }
    if (!manifest.flush()) throw IoError("error writing manifest");
    std::printf("extracted %zu entries (%s) to %s\n", range.last - range.first + 1, human_size(total).c_str(),
                out.string().c_str());
    return 0;
}

int cmd_pack(const fs::path& extracted, const fs::path& out) {
    std::ifstream manifest(extracted / kManifestName, std::ios::binary);
    if (!manifest) throw IoError("no " + std::string(kManifestName) + " in '" + extracted.string() + "'");

    struct Row {
        std::size_t index;
        std::string path;
    };
    std::vector<Row> rows;
    std::string line;
    std::size_t line_no = 0;
    bool header_seen = false;
    while (std::getline(manifest, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == kManifestHeader) header_seen = true;
        if (line.empty() || line.front() == '#') continue;
        std::istringstream fields(line);
        std::string index, type, size, path;
        if (!std::getline(fields, index, '\t') || !std::getline(fields, type, '\t') ||
            !std::getline(fields, size, '\t') || !std::getline(fields, path))
            throw FormatError("manifest line " + std::to_string(line_no) + ": expected 4 tab-separated fields");
        rows.push_back(Row{std::stoul(index), path});
    }
    if (!header_seen) throw FormatError("manifest is missing the '" + std::string(kManifestHeader) + "' header");

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].index != i)
            throw FormatError("manifest must list every index from 0 with no gaps (missing or duplicate " +
                              std::to_string(i) + "); extract the full archive before packing");

    fs::create_directories(out);
    pkg::Writer writer(pkg::output_paths(out));
    for (const auto& row : rows) {
        if (row.path == "-")
            writer.add({});
        else
            writer.add_file(extracted / row.path);
    }
    writer.finish();
    std::printf("packed %zu files into %s\n", writer.size(), out.string().c_str());
    return 0;
}

int cmd_verify(const fs::path& a_dir, const fs::path& b_dir) {
    pkg::Reader a(pkg::locate(a_dir));
    pkg::Reader b(pkg::locate(b_dir));
    if (a.size() != b.size()) {
        std::printf("file count differs: %zu vs %zu\n", a.size(), b.size());
        return 1;
    }

    constexpr std::size_t kChunk = 1 << 20;
    std::vector<std::byte> buf_a(kChunk), buf_b(kChunk);
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
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 2 : 0;
    }
    try {
        const auto cmd = args[0];
        if (cmd == "info" && args.size() == 2) return cmd_info(fs::path(args[1]));
        if (cmd == "list" && args.size() == 2) return cmd_list(fs::path(args[1]));
        if (cmd == "extract" && (args.size() == 3 || (args.size() == 5 && args[3] == "--range")))
            return cmd_extract(fs::path(args[1]), fs::path(args[2]),
                               args.size() == 5 ? std::optional(args[4]) : std::nullopt);
        if (cmd == "pack" && args.size() == 3) return cmd_pack(fs::path(args[1]), fs::path(args[2]));
        if (cmd == "verify" && args.size() == 3) return cmd_verify(fs::path(args[1]), fs::path(args[2]));
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccpkg: %s\n", e.what());
        return 1;
    }
}
