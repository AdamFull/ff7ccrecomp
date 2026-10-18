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
//
// `extract` needs no collection: model files (.bin: characters, monsters, weapons, stages) list their textures in
// tables of 16-byte records (u16 width, u16 height, u32 bits per texel 4|8, u32 palette offset, u32 texels offset;
// the palette is 16|256 RGBA8888 colours right before the texels), mostly swizzled (16-byte x 8-row blocks).
// Every texture found is decoded as the game draws it (checked against a collection: 618 of 618 alike) and added
// like an unpacked one, with its original picture in the collection folder (psptex pack --originals).
//
// One picture is listed once: the same pixels under another content hash (a texture drawn with leftovers of other
// palettes in the CLUT entries it never reaches, or a model shared by several files) are skipped; replacements
// are found by picture, so editing the one listed covers them all.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

#include "psprecore/io/bytes.hpp"
#include "psprecore/cli.hpp"
#include "psprecore/formats/cooked.hpp"
#include "psprecore/formats/png.hpp"
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
std::uint64_t key(const u8* p) {
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
    nx::vector<u8> texels;
    std::size_t signature = 0;  // offset of the bytes looked for (16-aligned, not all alike)
    bool searchable = false;
    // Where it was found.
    std::int64_t entry = -1;
    std::uint64_t offset = 0;  // of the texels in the entry
    std::size_t copies = 0;     // further matches
};

// Each resource's start in an Atel module, for naming where a texture lives.
nx::vector<std::uint64_t> resource_starts(std::span<const u8> file) {
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

// <assets>/index.tsv: its lines, the content hashes and the pictures (size and pixels hash) it lists.
struct Index {
    nx::string path;
    nx::vector<nx::string> lines;
    nx::hash_set<nx::string> hashes, pictures;

    explicit Index(const nx::string& assets) : path(path_join(assets, "index.tsv")), lines(cctex_lines(path)) {
        for (const nx::string& line : lines)
            if (!line.starts_with("#")) {
                const auto cols = split(line, '\t');
                if (cols.size() >= 2) hashes.insert(nx::string(cols[1]));
                if (cols.size() >= 5) pictures.insert(picture(cols[2], cols[3], cols[4]));
            }
        if (lines.empty()) {
            lines.push_back("# cctex: textures for editing; psptex pack turns edited pictures into replacements");
            lines.push_back("# path\tcontent hash\twidth\theight\toriginal\tkind\tentry\tresource\toffset");
        }
    }
    static nx::string picture(nx::string_view w, nx::string_view h, nx::string_view original) {
        return nx::format("{}x{}:{}", w, h, original);
    }
    bool write() const {
        nx::string text;
        for (const nx::string& line : lines) text += line + "\n";
        if (nx::fs::file_write_text(path, text).has_value()) return true;
        std::fprintf(stderr, "cctex: cannot write %s\n", path.c_str());
        return false;
    }
    static nx::vector<nx::string> cctex_lines(const nx::string& p) { return ::lines(p); }
};

int unpack(const nx::string& collection, const nx::string& game, const nx::string& assets) {
    // The collection.
    nx::vector<Texture> textures;
    for (const nx::string& line : lines(path_join(collection, "index.tsv"))) {
        const auto cols = split(line, '\t');
        if (cols.size() < 5 || cols[0].size() != 16) continue;
        Texture t{nx::string(cols[0]), std::atoi(nx::string(cols[1]).c_str()), std::atoi(nx::string(cols[2]).c_str()),
                  cols[4] == "2d"};
        try {
            t.texels = io::to_vector(value_or_throw(io::read_file(path_join(collection, t.hash + ".texels"))));
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
        const nx::vector<u8> file = reader.read(e);
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
    Index index(assets);
    nx::hash_map<std::int64_t, nx::vector<std::uint64_t>> resources;  // atel entries' resource starts
    std::size_t added = 0, found = 0, kept = 0, same_picture = 0;
    for (const Texture& t : textures) {
        if (t.entry >= 0) ++found;
        if (index.hashes.contains(t.hash)) {
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
                    const nx::vector<u8> file = reader.read(static_cast<std::size_t>(t.entry));
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
        const auto picture = psprecore::png::read(path_join(collection, t.hash + ".png"));
        if (!picture) {
            std::fprintf(stderr, "%s: no picture in the collection\n", t.hash.c_str());
            continue;
        }
        const int w = picture->w, h = picture->h;
        const std::uint64_t original = cooked::key_of(
            std::span(reinterpret_cast<const u8*>(picture->px.data()), static_cast<std::size_t>(w) * h * 4));
        if (!index.pictures.insert(Index::picture(nx::format("{}", w), nx::format("{}", h), nx::format("{:016X}", original)))
                 .second) {
            ++same_picture;
            continue;
        }
        (void)nx::fs::create_directories(parent_path(target));
        if (!nx::fs::exists(target)) (void)nx::fs::copy(path_join(collection, t.hash + ".png"), target, false);
        index.lines.push_back(nx::format("{}\t{}\t{}\t{}\t{:016X}\t{}\t{}\t{}\t{:X}", relative, t.hash, w, h, original,
                                   t.two_d ? "2d" : "3d", t.entry >= 0 ? nx::format("{:05}", t.entry) : nx::string("-"),
                                   resource, offset));
        ++added;
    }
    if (!index.write()) return 1;
    std::fprintf(stderr,
                 "%zu found in the archive, %zu made at run time; %zu added to %s (%zu listed before, %zu the same "
                 "picture as one listed)\n",
                 found, textures.size() - found, added, assets.c_str(), kept, same_picture);
    return 0;
}

// A texture of a model file's table, decoded: its RGBA8888 palette colours by texel, plain or swizzled.
void decode(std::span<const u8> file, int w, int h, int bits, std::uint32_t palette, std::uint32_t texels,
            bool swizzled, nx::vector<std::uint32_t>& out) {
    const std::size_t pitch = static_cast<std::size_t>(w) * static_cast<std::size_t>(bits) / 8;
    const u8* src = file.data() + texels;
    const auto colour = [&](unsigned i) {
        std::uint32_t c;
        std::memcpy(&c, file.data() + palette + i * 4, 4);
        return c;
    };
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const auto yy = static_cast<std::size_t>(y);
        std::uint32_t* row = out.data() + yy * static_cast<std::size_t>(w);
        for (std::size_t x = 0; x < pitch; ++x) {
            // Swizzled: 16-byte x 8-row blocks, row by row.
            const std::size_t at = swizzled ? ((yy / 8 * (pitch / 16) + x / 16) * 8 + yy % 8) * 16 + x % 16 : yy * pitch + x;
            const auto byte = static_cast<unsigned>(src[at]);
            if (bits == 8) {
                row[x] = colour(byte);
            } else {
                row[x * 2] = colour(byte & 15u);
                row[x * 2 + 1] = colour(byte >> 4);
            }
        }
    }
}

// How much neighbouring texels differ (mean over R, G, B, right and below): a texture read the wrong way round is
// noise.
double roughness(const nx::vector<std::uint32_t>& px, int w, int h) {
    std::uint64_t sum = 0, n = 0;
    const auto diff = [](std::uint32_t a, std::uint32_t b) {
        int d = 0;
        for (int s = 0; s < 24; s += 8) d += std::abs(static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF));
        return static_cast<std::uint64_t>(d);
    };
    const auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x + 1 < w) sum += diff(at(x, y), at(x + 1, y)), n += 3;
            if (y + 1 < h) sum += diff(at(x, y), at(x, y + 1)), n += 3;
        }
    return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
}

constexpr double kNoise = 40.0;  // roughness of a texture decoded wrongly or of data that is no texture (real: below)

int extract(const nx::string& game, const nx::string& assets, const nx::string& collection) {
    pkg::Reader reader(pkg::locate(game));
    Index index(assets);
    (void)nx::fs::create_directories(collection);
    std::size_t tables = 0, added = 0, same_picture = 0, noise = 0, files = 0;
    nx::vector<std::uint32_t> plain, swizzled;
    for (std::size_t e = 0; e < reader.size(); ++e) {
        const nx::vector<u8> file = reader.read(e);
        const nx::string folder = nx::format(
            "{:05}{}", e, pkg::type_extension(pkg::sniff(std::span(file).first(std::min<std::size_t>(file.size(), 16)))));
        if (!folder.ends_with(".bin")) continue;
        nx::hash_set<std::uint32_t> seen;  // texels offsets (a table can list a texture more than once)
        bool any = false;
        for (std::size_t at = 0; at + 16 <= file.size(); at += 4) {
            std::uint16_t w = 0, h = 0;
            std::uint32_t bits = 0, palette = 0, texels = 0;
            std::memcpy(&w, file.data() + at, 2);
            std::memcpy(&h, file.data() + at + 2, 2);
            std::memcpy(&bits, file.data() + at + 4, 4);
            std::memcpy(&palette, file.data() + at + 8, 4);
            std::memcpy(&texels, file.data() + at + 12, 4);
            const auto pow2 = [](unsigned v) { return v >= 8 && v <= 512 && (v & (v - 1)) == 0; };
            if (!pow2(w) || !pow2(h) || (bits != 4 && bits != 8) || palette == 0) continue;
            if (texels < palette || texels - palette != (bits == 4 ? 64u : 1024u)) continue;
            const std::size_t size = std::size_t{w} * h * bits / 8;
            if (texels + size > file.size() || !seen.insert(texels).second) continue;
            ++tables;
            // Plain or swizzled: whichever reads as a picture.
            decode(file, w, h, static_cast<int>(bits), palette, texels, false, plain);
            double best = roughness(plain, w, h);
            const nx::vector<std::uint32_t>* px = &plain;
            if (std::size_t{w} * bits / 8 >= 16 && h % 8 == 0) {
                decode(file, w, h, static_cast<int>(bits), palette, texels, true, swizzled);
                if (const double r = roughness(swizzled, w, h); r < best) {
                    best = r;
                    px = &swizzled;
                }
            }
            if (best > kNoise) {
                ++noise;
                continue;
            }
            const nx::string original = nx::format("{:016X}", cooked::key_of(psprecore::io::bytes_of(std::span(*px))));
            if (!index.pictures.insert(Index::picture(nx::format("{}", w), nx::format("{}", h), original)).second) {
                ++same_picture;
                continue;
            }
            // Its content hash: palette and texels (a collected texture's has the CLUT as the game loaded it instead;
            // replacements are found by picture either way).
            const nx::string hash =
                nx::format("{:016X}", cooked::key_of(std::span(file.data() + palette, texels - palette + size)));
            const nx::string relative = nx::format("{}/{:06X}_{}.png", folder, texels, hash);
            const nx::string target = path_join(assets, relative);
            (void)nx::fs::create_directories(parent_path(target));
            const auto save = [&](const nx::string& path) {
                return nx::fs::exists(path) || psprecore::png::write(path, px->data(), w, h).has_value();
            };
            if (!save(target) || !save(path_join(collection, hash + ".png"))) {
                std::fprintf(stderr, "cctex: cannot write %s\n", target.c_str());
                return 1;
            }
            index.hashes.insert(hash);
            index.lines.push_back(
                nx::format("{}\t{}\t{}\t{}\t{}\t3d\t{:05}\t-\t{:X}", relative, hash, w, h, original, e, texels));
            ++added;
            any = true;
        }
        if (any) ++files;
    }
    if (!index.write()) return 1;
    std::fprintf(stderr,
                 "%zu textures in model tables: %zu added to %s from %zu files, %zu the same picture as one listed, "
                 "%zu not pictures (noise)\n",
                 tables, added, assets.c_str(), files, same_picture, noise);
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
    cli.command("extract", "the model files' textures (no collection needed) -> <assets>, originals -> <collection>")
        .positional("game", game, "the game (disc folder)")
        .positional("assets", assets, "folder for the editable pictures (e.g. assets/textures)")
        .positional("collection", collection, "texture collection folder (psptex pack --originals)");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "unpack") return unpack(collection, game, assets);
        if (command == "extract") return extract(game, assets, collection);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctex: %s\n", e.what());
        return 1;
    }
}
