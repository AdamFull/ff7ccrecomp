// cctex — the game's textures unpacked for editing by hand; psptex pack (run by the build) turns edited pictures into
// replacements the game draws instead (data/cooked/textures).
//
// `unpack` takes a texture collection (ff7cc --tex-collect DIR: the textures the game drew, as pictures and as the
// raw texels the GE read) and finds each texture in the game's archive: the entry and the offset of its texels, and
// in script modules the resource holding them. Its picture is copied to
//
//     <assets>/<entry>.<type>[/r<resource>]/<offset>_<content hash>.png
//
// and listed in <assets>/index.tsv (psprecore/tools/psptex: path, content hash, width, height, the original
// pixels' hash; then 2d|3d, archive entry, resource, offset). Pictures already there are never overwritten (they
// may be edited); index lines are kept. Textures not found in the archive (made by the game while it runs) go to
// runtime/. Collect more by playing or replaying with --tex-collect, then unpack again.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "psprecore/cli.hpp"
#include "psprecore/formats/cooked.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;

namespace {

constexpr std::size_t kSignature = 32;  // bytes compared to find a texture

nx::vector<nx::string_view> split(nx::string_view line, char sep) {
    nx::vector<nx::string_view> out;
    while (true) {
        const std::size_t end = line.find(sep);
        out.push_back(line.substr(0, end));
        if (end == nx::string_view::npos) return out;
        line.remove_prefix(end + 1);
    }
}

nx::vector<nx::string> lines(const nx::string& path) {
    nx::vector<nx::string> out;
    const auto text = nx::fs::file_read_text(path);
    if (!text.has_value()) return out;
    for (nx::string_view line : split(text.value(), '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) out.emplace_back(line);
    }
    return out;
}

// A fast key for kSignature bytes (exact matches are checked afterwards).
std::uint64_t key(const std::byte* p) {
    std::uint64_t w[4];
    std::memcpy(w, p, sizeof w);
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    for (std::uint64_t v : w) h = (h ^ v) * 0x100000001B3ull + (h >> 29);
    return h;
}

struct Texture {
    nx::string hash;
    int width = 0, height = 0;
    bool two_d = false;
    nx::vector<std::byte> texels;
    std::size_t signature = 0;  // offset of the bytes looked for (16-aligned, not all alike)
    bool searchable = false;
    // Where it was found.
    std::int64_t entry = -1;
    std::uint64_t offset = 0;  // of the texels in the entry
    std::size_t copies = 0;     // further matches
};

// Each resource's start in an Atel module, for naming where a texture lives.
nx::vector<std::uint64_t> resource_starts(std::span<const std::byte> file) {
    nx::vector<std::uint64_t> starts;
    try {
        const atel::Module m = atel::parse(file);
        std::uint64_t total = 0;
        for (const auto& r : m.resources) total += r.data.size() + r.slack.size();
        std::uint64_t at = file.size() - total;
        for (const auto& r : m.resources) {
            starts.push_back(at);
            at += r.data.size() + r.slack.size();
        }
    } catch (const std::exception&) {
    }
    return starts;
}

int unpack(const nx::string& collection, const nx::string& game, const nx::string& assets) {
    // The collection.
    nx::vector<Texture> textures;
    for (const nx::string& line : lines(path_join(collection, "index.tsv"))) {
        const auto cols = split(line, '\t');
        if (cols.size() < 5 || cols[0].size() != 16) continue;
        Texture t{nx::string(cols[0]), std::atoi(nx::string(cols[1]).c_str()), std::atoi(nx::string(cols[2]).c_str()),
                  cols[4] == "2d"};
        try {
            t.texels = io::read_all(path_join(collection, t.hash + ".texels"));
        } catch (const std::exception&) {
        }
        for (std::size_t at = 0; at + kSignature <= t.texels.size(); at += 16) {
            nx::hash_set<std::uint8_t> distinct;
            for (std::size_t i = 0; i < kSignature; ++i) distinct.insert(static_cast<std::uint8_t>(t.texels[at + i]));
            if (distinct.size() >= 6) {
                t.signature = at;
                t.searchable = true;
                break;
            }
        }
        textures.push_back(std::move(t));
    }
    nx::hash_map<std::uint64_t, nx::vector<std::size_t>> wanted;
    for (std::size_t i = 0; i < textures.size(); ++i)
        if (textures[i].searchable) wanted[key(textures[i].texels.data() + textures[i].signature)].push_back(i);
    std::fprintf(stderr, "%zu textures collected, %zu with texels to look for\n", textures.size(), wanted.size());

    // One pass over the archive: every 16-byte aligned position.
    pkg::Reader reader(pkg::locate(game));
    nx::vector<nx::string> folder(reader.size());
    for (std::size_t e = 0; e < reader.size(); ++e) {
        const nx::vector<std::byte> file = reader.read(e);
        folder[e] = nx::format("{:05}{}", e, pkg::type_extension(pkg::sniff(std::span(file).first(std::min<std::size_t>(file.size(), 16)))));
        for (std::size_t at = 0; at + kSignature <= file.size(); at += 16) {
            const auto it = wanted.find(key(file.data() + at));
            if (it == wanted.end()) continue;
            for (const std::size_t i : it->second) {
                Texture& t = textures[i];
                if (at < t.signature || at - t.signature + t.texels.size() > file.size()) continue;
                const std::size_t start = at - t.signature;
                if (std::memcmp(file.data() + start, t.texels.data(), t.texels.size()) != 0) continue;
                if (t.entry < 0) {
                    t.entry = static_cast<std::int64_t>(e);
                    t.offset = start;
                } else {
                    ++t.copies;
                }
            }
        }
    }

    // The pictures, laid out by where they come from.
    const nx::string index_path = path_join(assets, "index.tsv");
    nx::vector<nx::string> index = lines(index_path);
    nx::hash_set<nx::string> listed;
    for (const nx::string& line : index)
        if (!line.starts_with("#")) {
            const auto cols = split(line, '\t');
            if (cols.size() >= 2) listed.insert(nx::string(cols[1]));
        }
    if (index.empty()) {
        index.push_back("# cctex unpack: textures for editing; psptex pack turns edited pictures into replacements");
        index.push_back("# path\tcontent hash\twidth\theight\toriginal\tkind\tentry\tresource\toffset");
    }
    nx::hash_map<std::int64_t, nx::vector<std::uint64_t>> resources;  // atel entries' resource starts
    std::size_t added = 0, found = 0, kept = 0;
    for (const Texture& t : textures) {
        if (t.entry >= 0) ++found;
        if (listed.contains(t.hash)) {
            ++kept;
            continue;
        }
        nx::string dir = "runtime", resource = "-";
        std::uint64_t offset = 0;
        if (t.entry >= 0) {
            dir = folder[static_cast<std::size_t>(t.entry)];
            offset = t.offset;
            if (dir.ends_with(".atel")) {
                auto it = resources.find(t.entry);
                if (it == resources.end()) {
                    const nx::vector<std::byte> file = reader.read(static_cast<std::size_t>(t.entry));
                    it = resources.emplace(t.entry, resource_starts(file)).first;
                }
                const auto& starts = it->second;
                const auto r = std::upper_bound(starts.begin(), starts.end(), t.offset);
                if (r != starts.begin()) {
                    const std::size_t n = static_cast<std::size_t>(r - starts.begin()) - 1;
                    resource = nx::format("{}", n);
                    dir = nx::format("{}/r{}", dir, n);
                    offset = t.offset - starts[n];
                }
            }
        }
        const nx::string relative = nx::format("{}/{:06X}_{}.png", dir, offset, t.hash);
        const nx::string target = path_join(assets, relative);
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(path_join(collection, t.hash + ".png").c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            std::fprintf(stderr, "%s: no picture in the collection\n", t.hash.c_str());
            continue;
        }
        const std::uint64_t original =
            cooked::fnv1a64(std::span(reinterpret_cast<const std::byte*>(pixels), static_cast<std::size_t>(w) * h * 4));
        stbi_image_free(pixels);
        (void)nx::fs::create_directories(parent_path(target));
        if (!nx::fs::exists(target)) (void)nx::fs::copy(path_join(collection, t.hash + ".png"), target, false);
        index.push_back(nx::format("{}\t{}\t{}\t{}\t{:016X}\t{}\t{}\t{}\t{:X}", relative, t.hash, w, h, original,
                                   t.two_d ? "2d" : "3d", t.entry >= 0 ? nx::format("{:05}", t.entry) : nx::string("-"),
                                   resource, offset));
        ++added;
    }
    nx::string text;
    for (const nx::string& line : index) text += line + "\n";
    if (!nx::fs::file_write_text(index_path, text).has_value()) {
        std::fprintf(stderr, "cctex: cannot write %s\n", index_path.c_str());
        return 1;
    }
    std::fprintf(stderr, "%zu found in the archive, %zu made at run time; %zu added to %s (%zu listed before)\n", found,
                 textures.size() - found, added, assets.c_str(), kept);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    nx::string collection, game, assets;
    nx::cli::Parser cli("cctex", "The game's textures unpacked for editing (see psptex pack for the way back).");
    cli.command("unpack", "a texture collection (ff7cc --tex-collect) -> <assets> by archive entry, with index.tsv")
        .positional("collection", collection, "texture collection folder")
        .positional("game", game, "the game (disc folder)")
        .positional("assets", assets, "folder for the editable pictures (e.g. assets/textures)");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "unpack") return unpack(collection, game, assets);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctex: %s\n", e.what());
        return 1;
    }
}
