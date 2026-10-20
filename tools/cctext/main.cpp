// cctext — the game's dialogue and menu text (MBD tables in Atel modules) as gettext PO files.
//
//   cctext export <game> <out.po>             every message; msgctxt "EEEEE.RR.MMM" = entry, resource, message
//   cctext build  <game> <in.po> <mod dir> [--language NAME]
//                                              modules whose translated text differs -> <mod dir>/pkg/EEEEE.atel;
//                                              with --language, a language pack (<mod dir>/mod.ini) chosen in the
//                                              pause menu
//   cctext verify <game>                      binary and text round trips of every message
//   cctext font-export <game> <out.png>       the regular font's sheet as a picture to draw new characters in
//   cctext font-import <game> <in.png> <mod dir>  the edited sheet -> <mod dir>/pkg/01472.bin
//                                              (both with --small: the small font's sheet, which menus use)
//   cctext names-export <game> <out.po>       the name table: locations, speakers, mission titles; msgctxt
//                                              "names.OOOOO" = offset of the field (hex)
//   cctext names-build <game> <in.po> <mod dir>  the translated name table -> <mod dir>/pkg/00001.bin
//   cctext menus-export <game> <out.po>       menu text: items, materia, commands, abilities ("menus.EEEEE.RRR.name"
//                                              or ".desc"), missions and mail ("menus.EEEEE.OOOOO", field offset),
//                                              enemies and actions in battle modules ("battle.EEEEE.NNN")
//   cctext menus-build <game> <in.po> <mod dir>  the translated menu text -> <mod dir>/pkg/EEEEE.bin|.atel
//
// With <mod dir>/font/glyphs.txt (ff7cc/formats/font.hpp), build also encodes the characters the mod adds.
//
// See docs/localization.md for the text syntax ({PAGE}, {CHOICE:200}, {C10}, {G:150}).

#include <cstdio>
#include <exception>
#include <optional>

#include "psprecore/error.hpp"
#include "psprecore/formats/png.hpp"
#include "psprecore/cli.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/font.hpp"
#include "ff7cc/formats/mbd.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "ff7cc/formats/sections.hpp"
#include "ff7cc/formats/strings.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;

namespace {

struct Key {
    std::size_t entry = 0, resource = 0, message = 0;
    auto operator<=>(const Key&) const = default;
};

nx::string key_string(const Key& k) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%05zu.%02zu.%03zu", k.entry, k.resource, k.message);
    return buf;
}

std::optional<Key> parse_key(nx::string_view s) {
    Key k;
    if (std::sscanf(nx::string(s).c_str(), "%zu.%zu.%zu", &k.entry, &k.resource, &k.message) != 3) return std::nullopt;
    return k;
}

// Calls fn(entry, module, resource index, messages) for every MBD resource of every Atel module.
template <class Fn>
void for_each_table(pkg::Reader& reader, Fn&& fn) {
    for (std::size_t i = 0; i < reader.size(); ++i) {
        if (reader.sniff(i) != pkg::FileType::Atel) continue;
        const auto data = reader.read(i);
        atel::Module module = atel::parse(data);
        for (std::size_t r = 0; r < module.resources.size(); ++r)
            if (mbd::is_mbd(module.resources[r].data)) fn(i, module, r, mbd::parse(module.resources[r].data));
    }
}

// MBD tables outside script modules (resource numbers for the PO): entry 1's sections 3, 5 and 7 (system messages,
// "Obtained ...!", the main menu's help lines; ff7cc/formats/sections.hpp), and the tables that end entries 1435
// (mission descriptions) and 1436 (mail), resource 0. They grow like a module's (mbd::patch keeps every message's
// place); entry 1's later sections move with them.
constexpr std::size_t kEmbeddedEntries[] = {1, 1435, 1436};

struct EmbeddedTable {
    std::size_t resource = 0, offset = 0, size = 0;
};

nx::vector<EmbeddedTable> embedded_tables(std::size_t entry, std::span<const u8> file) {
    nx::vector<EmbeddedTable> out;
    if (entry == 1) {
        const auto list = sections::parse(file);
        for (const std::size_t r : {3, 5, 7})
            if (r < list.size() && mbd::is_mbd(file.subspan(list[r].offset, list[r].size)))
                out.push_back({r, list[r].offset, list[r].size});
    } else if (std::ranges::find(kEmbeddedEntries, entry) != std::end(kEmbeddedEntries)) {
        const auto it = std::ranges::search(file, std::span<const u8>(reinterpret_cast<const u8*>("MBD"), 4));
        if (!it.empty()) {
            const auto at = static_cast<std::size_t>(it.begin() - file.begin());
            out.push_back({0, at, file.size() - at});
        }
    }
    return out;
}

// Calls fn(entry, resource, messages) for the tables of embedded_tables.
template <class Fn>
void for_each_embedded(pkg::Reader& reader, Fn&& fn) {
    for (const std::size_t entry : kEmbeddedEntries) {
        const auto data = reader.read(entry);
        for (const auto& t : embedded_tables(entry, data))
            fn(entry, t.resource, mbd::parse(std::span(data).subspan(t.offset, t.size)));
    }
}

// ---- PO files ------------------------------------------------------------------------------------------------

nx::string po_escape(nx::string_view s) {
    nx::string out;
    for (const char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// A PO string: one line, or "" followed by one line per text line when it has newlines.
nx::string po_string(nx::string_view s) {
    if (s.find('\n') == nx::string_view::npos || s.find('\n') == s.size() - 1) return "\"" + po_escape(s) + "\"";
    nx::string out = "\"\"";
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t nl = s.find('\n', start);
        const std::size_t end = nl == nx::string_view::npos ? s.size() : nl + 1;
        out += "\n\"" + po_escape(s.substr(start, end - start)) + "\"";
        start = end;
    }
    return out;
}

nx::string po_unescape(nx::string_view s) {
    nx::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            default: out += s[i]; break;
        }
    }
    return out;
}

struct PoEntry {
    nx::string context, id, str;
};

nx::vector<PoEntry> read_po(nx::fs::path_view path) {
    const auto file = nx::fs::file_read_text(path);
    if (!file) throw Exception(nx::string("cannot read ") + path.native());
    nx::vector<PoEntry> out;
    PoEntry cur;
    nx::string* field = nullptr;
    const auto flush = [&] {
        if (!cur.context.empty() || !cur.id.empty()) out.push_back(cur);
        cur = PoEntry{};
        field = nullptr;
    };
    const auto quoted = [](nx::string_view line) {
        const auto a = line.find('"'), b = line.rfind('"');
        if (a == nx::string_view::npos || b <= a)
            throw Exception(nx::string("PO: expected a quoted string: ") + line);
        return po_unescape(line.substr(a + 1, b - a - 1));
    };
    const nx::string_view text = file.value();
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == nx::string_view::npos ? text.size() : nl;
        nx::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) {
            flush();
            continue;
        }
        if (line[0] == '#') continue;
        if (line.starts_with("msgctxt ")) {
            if (field && field != &cur.context) flush();
            cur.context = quoted(line);
            field = &cur.context;
        } else if (line.starts_with("msgid ")) {
            cur.id = quoted(line);
            field = &cur.id;
        } else if (line.starts_with("msgstr ")) {
            cur.str = quoted(line);
            field = &cur.str;
        } else if (line[0] == '"' && field) {
            *field += quoted(line);
        }
    }
    flush();
    return out;
}

// ---- commands ------------------------------------------------------------------------------------------------

int export_po(nx::fs::path_view game, nx::fs::path_view out_path) {
    pkg::Reader reader(pkg::locate(game));
    const auto charset = mbd::Charset::european();
    nx::string out =
        "# Crisis Core -Final Fantasy VII- text (FF7CCRECOMP cctext). msgctxt: archive entry.resource.message\n"
        "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n";
    std::size_t messages = 0, tables = 0;
    const auto add = [&](std::size_t entry, std::size_t r, const nx::vector<mbd::Message>& table) {
        ++tables;
        for (std::size_t m = 0; m < table.size(); ++m) {
            out += "\nmsgctxt \"" + key_string({entry, r, m}) + "\"\nmsgid " + po_string(mbd::to_text(table[m], charset)) +
                   "\nmsgstr \"\"\n";
            ++messages;
        }
    };
    for_each_table(reader, [&](std::size_t entry, const atel::Module&, std::size_t r, const nx::vector<mbd::Message>& table) {
        add(entry, r, table);
    });
    for_each_embedded(reader, add);
    if (!nx::fs::file_write_text(out_path, out)) throw Exception(nx::string("cannot write ") + out_path.native());
    std::fprintf(stderr, "%zu messages from %zu tables -> %s\n", messages, tables, nx::string(out_path.native()).c_str());
    return 0;
}

int verify(nx::fs::path_view game) {
    pkg::Reader reader(pkg::locate(game));
    const auto charset = mbd::Charset::european();
    std::size_t tables = 0, messages = 0, failures = 0, unnamed = 0;
    for_each_table(reader, [&](std::size_t entry, const atel::Module& module, std::size_t r, const nx::vector<mbd::Message>& table) {
        ++tables;
        if (mbd::serialize(table) != module.resources[r].data) {
            ++failures;
            std::fprintf(stderr, "binary round trip differs: entry %zu resource %zu\n", entry, r);
        }
        for (std::size_t m = 0; m < table.size(); ++m, ++messages) {
            const nx::string text = mbd::to_text(table[m], charset);
            if (text.contains("{G:")) ++unnamed;
            if (mbd::from_text(text, charset) != table[m]) {
                ++failures;
                std::fprintf(stderr, "text round trip differs: %s\n", key_string({entry, r, m}).c_str());
            }
        }
    });
    for (const std::size_t entry : kEmbeddedEntries) {  // tables outside modules, and entry 1 rebuilt around them
        const auto data = reader.read(entry);
        for (const auto& t : embedded_tables(entry, data)) {
            const auto table = std::span<const u8>(data).subspan(t.offset, t.size);
            const auto again = mbd::serialize(mbd::parse(table));
            const bool same = std::ranges::equal(again, table);
            if (!same || (entry == 1 && sections::replace(data, t.resource, table) != data)) {
                ++failures;
                std::fprintf(stderr, "embedded table round trip differs: entry %zu table %zu\n", entry, t.resource);
            }
        }
    }
    for (const std::size_t entry : strings::kTableEntries) {  // menu text: string tables rebuilt as they were
        const auto data = reader.read(entry);
        if (strings::serialize(strings::parse_table(data)) != data) {
            ++failures;
            std::fprintf(stderr, "string table round trip differs: entry %zu\n", entry);
        }
    }
    std::fprintf(stderr, "%zu tables, %zu messages (%zu use unnamed glyphs), %zu failures\n", tables, messages, unnamed,
                 failures);
    return failures ? 1 : 0;
}

// The game's characters plus those the mod adds (<mod dir>/font/glyphs.txt), if any.
mbd::Charset mod_charset(nx::fs::path_view mod_dir) {
    mbd::Charset charset = mbd::Charset::european();
    const nx::string path = path_join(mod_dir.native(), "font/glyphs.txt");
    if (!is_file(path)) return charset;
    const auto text = nx::fs::file_read_text(path);
    if (!text.has_value()) throw Exception("cannot read '" + path + "'");
    for (const auto& g : font::parse_glyphs(text.value())) {
        if (g.advance) charset.glyphs[static_cast<std::size_t>(g.index)] = g.text;
        else charset.aliases.emplace_back(g.text, static_cast<std::uint16_t>(g.index));
    }
    std::fprintf(stderr, "characters from %s\n", path.c_str());
    return charset;
}

// An archive entry as the mod has it already (<pkg dir>/EEEEE.<type>, written by an earlier command of the same
// build), else the game's.
nx::vector<u8> mod_or_game(pkg::Reader& reader, const nx::string& pkg_dir, std::size_t entry, const char* type) {
    const nx::string own = path_join(pkg_dir, nx::format("{:05}.{}", entry, type));
    if (!is_file(own)) return reader.read(entry);
    const auto blob = value_or_throw(io::read_file(own));
    return nx::vector<u8>(blob.begin(), blob.end());
}

// ---- the font sheets -------------------------------------------------------------------------------------------

struct FontFile {
    nx::vector<u8> data;
    font::Picture sheet;
    nx::vector<std::uint32_t> palette;
};

// The regular font's sheet, or the small font's (menus); from the mod's font file when `pkg_dir` has one.
FontFile read_font(nx::fs::path_view game, bool small, const nx::string& pkg_dir = {}) {
    pkg::Reader reader(pkg::locate(game));
    FontFile f;
    f.data = pkg_dir.empty() ? reader.read(font::kFontEntry) : mod_or_game(reader, pkg_dir, font::kFontEntry, "bin");
    const auto items = font::items(f.data);
    if (items.size() <= font::kSmallPalette) throw Exception("font: unexpected layout");
    f.sheet = font::picture(f.data, items[small ? font::kSmallSheet : font::kRegularSheet]);
    f.palette = font::palette(f.data, items[small ? font::kSmallPalette : font::kTextPalette]);
    return f;
}

int font_export(nx::fs::path_view game, const nx::string& png, bool small) {
    const FontFile f = read_font(game, small);
    const auto indices = font::unswizzle4(std::span(f.data).subspan(f.sheet.texels, f.sheet.bytes), f.sheet.width,
                                          f.sheet.height);
    nx::vector<std::uint32_t> rgba(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) rgba[i] = f.palette[indices[i]];
    if (const auto written = psprecore::png::write(png, rgba.data(), f.sheet.width, f.sheet.height); !written)
        throw Exception(written.error());
    if (small)
        std::fprintf(stderr, "%dx%d small sheet -> %s (cells %dx%d, %d to a row, a copy %d texels lower; free: glyphs "
                     "96-113, 229-275)\n", f.sheet.width, f.sheet.height, png.c_str(), font::kSmallCellWidth,
                     font::kSmallCellHeight, font::kSmallColumns, font::kSmallCopyOffset);
    else
        std::fprintf(stderr, "%dx%d sheet -> %s (cells %dx%d, %d to a row; free: glyphs 96-113, 228-341)\n",
                     f.sheet.width, f.sheet.height, png.c_str(), font::kCellWidth, font::kCellHeight, font::kColumns);
    return 0;
}

// Each pixel becomes the palette colour nearest to it, so new characters should be drawn with the sheet's own
// colours (copy them from existing glyphs). Into the mod's font file when it has one (both sheets edited).
int font_import(nx::fs::path_view game, const nx::string& png, nx::fs::path_view mod_dir, bool small) {
    FontFile f = read_font(game, small, path_join(mod_dir.native(), "pkg"));
    auto picture = psprecore::png::read(png);
    if (!picture) throw Exception(picture.error());
    const nx::vector<u32> rgba = std::move(picture->px);
    const int w = picture->w, h = picture->h;
    if (w != f.sheet.width || h != f.sheet.height)
        throw Exception(nx::format("the picture must be {}x{} like the sheet", f.sheet.width, f.sheet.height));
    const auto channel = [](std::uint32_t c, int i) { return static_cast<int>((c >> (i * 8)) & 0xFF); };
    nx::vector<std::uint8_t> indices(rgba.size());
    std::size_t inexact = 0;
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        int best = 0, best_distance = 1 << 30;
        for (int p = 0; p < 16; ++p) {
            int d = 0;
            for (int k = 0; k < 4; ++k) {
                const int diff = channel(rgba[i], k) - channel(f.palette[static_cast<std::size_t>(p)], k);
                d += diff * diff;
            }
            if (channel(rgba[i], 3) == 0 && channel(f.palette[static_cast<std::size_t>(p)], 3) == 0) d = 0;  // transparent
            if (d < best_distance) {
                best_distance = d;
                best = p;
            }
        }
        if (best_distance) ++inexact;
        indices[i] = static_cast<std::uint8_t>(best);
    }
    font::swizzle4(indices, f.sheet.width, f.sheet.height, std::span(f.data).subspan(f.sheet.texels, f.sheet.bytes));
    const nx::string pkg_dir = path_join(mod_dir.native(), "pkg");
    if (!nx::fs::create_directories(pkg_dir)) throw Exception("cannot create directory '" + pkg_dir + "'");
    char name[16];
    std::snprintf(name, sizeof name, "%05zu.bin", font::kFontEntry);
    value_or_throw(io::write_file(path_join(pkg_dir, name), f.data));
    std::fprintf(stderr, "%s -> %s (%zu pixels not in the palette, mapped to the nearest colour)\n", png.c_str(),
                 path_join(pkg_dir, name).c_str(), inexact);
    return 0;
}

int build(nx::fs::path_view game, nx::fs::path_view po_path, nx::fs::path_view mod_dir, const nx::string& language) {
    const auto charset = mod_charset(mod_dir);
    nx::map<Key, nx::string> translated;
    for (const auto& e : read_po(po_path)) {
        if (e.context.empty() || e.str.empty() || e.str == e.id) continue;
        const auto key = parse_key(e.context);
        if (!key) throw Exception("PO: bad msgctxt " + e.context);
        translated[*key] = e.str;
    }
    pkg::Reader reader(pkg::locate(game));
    nx::map<std::size_t, nx::vector<std::pair<Key, nx::string>>> by_entry;
    for (const auto& [k, s] : translated) by_entry[k.entry].emplace_back(k, s);

    const nx::string pkg_dir = path_join(mod_dir.native(), "pkg");
    if (!nx::fs::create_directories(pkg_dir)) throw Exception("cannot create directory '" + pkg_dir + "'");
    std::size_t modules = 0, changed = 0, errors = 0;
    for (const auto& [entry, items] : by_entry) {
        if (std::ranges::find(kEmbeddedEntries, entry) != std::end(kEmbeddedEntries)) {
            // Into the mod's copy when an earlier step wrote one (names-build: entry 1's name table).
            nx::vector<u8> data = mod_or_game(reader, pkg_dir, entry, "bin");
            nx::map<std::size_t, nx::map<std::size_t, mbd::Message>> changes;
            for (const auto& [k, text] : items) {
                try {
                    changes[k.resource][k.message] = mbd::from_text(text, charset);
                    ++changed;
                } catch (const Exception& e) {
                    ++errors;
                    std::fprintf(stderr, "%s: %s\n", key_string(k).c_str(), e.what());
                }
            }
            for (const auto& [r, messages] : changes) {
                const auto tables = embedded_tables(entry, data);
                const auto t = std::ranges::find(tables, r, &EmbeddedTable::resource);
                if (t == tables.end()) throw Exception(nx::format("PO: entry {} has no text table {}", entry, r));
                const auto table = std::span<const u8>(data).subspan(t->offset, t->size);
                if (messages.rbegin()->first >= mbd::parse(table).size())
                    throw Exception(nx::format("PO: entry {} table {}: a message past its end", entry, r));
                const nx::vector<u8> patched = mbd::patch(table, messages);
                if (entry == 1) {
                    data = sections::replace(data, r, patched);
                } else {  // (the table ends the file)
                    data.resize(t->offset);
                    data.insert(data.end(), patched.begin(), patched.end());
                }
            }
            value_or_throw(io::write_file(path_join(pkg_dir, nx::format("{:05}.bin", entry)), data));
            ++modules;
            continue;
        }
        if (entry >= reader.size() || reader.sniff(entry) != pkg::FileType::Atel)
            throw Exception("PO: entry " + nx::format("{}", entry) + " is not a script module");
        const auto data = reader.read(entry);
        atel::Module module = atel::parse(data);
        // Scripts address messages and resources by offset: changed messages are patched in place (mbd::patch) and
        // the tables replaced without moving other resources (atel::replace_resources).
        nx::map<std::size_t, nx::map<std::size_t, mbd::Message>> changes;  // resource -> message -> content
        for (const auto& [k, text] : items) {
            if (k.resource >= module.resources.size() || !mbd::is_mbd(module.resources[k.resource].data))
                throw Exception("PO: " + key_string(k) + " is not a text table");
            const auto count = mbd::parse(module.resources[k.resource].data).size();
            if (k.message >= count) throw Exception("PO: " + key_string(k) + " is past the end of its table");
            try {
                changes[k.resource][k.message] = mbd::from_text(text, charset);
                ++changed;
            } catch (const Exception& e) {
                ++errors;
                std::fprintf(stderr, "%s: %s\n", key_string(k).c_str(), e.what());
            }
        }
        nx::map<std::size_t, nx::vector<u8>> replacements;
        for (const auto& [r, messages] : changes) replacements[r] = mbd::patch(module.resources[r].data, messages);
        char name[16];
        std::snprintf(name, sizeof name, "%05zu.atel", entry);
        value_or_throw(io::write_file(path_join(pkg_dir, name), atel::replace_resources(data, replacements)));
        ++modules;
    }
    std::fprintf(stderr, "%zu messages changed in %zu modules -> %s (%zu errors)\n", changed, modules,
                 pkg_dir.c_str(), errors);
    if (!language.empty()) {  // a language pack (psprecore/platform/mods.hpp)
        const nx::string ini = path_join(mod_dir.native(), "mod.ini");
        if (!nx::fs::file_write_text(ini, nx::format("name={} translation\nlanguage={}\n", language, language)).has_value())
            throw Exception("cannot write '" + ini + "'");
        std::fprintf(stderr, "language pack: %s (%s)\n", language.c_str(), ini.c_str());
    }
    return errors ? 1 : 0;
}

// A one-line translation as glyph indices (names, menu text: no pages or control codes). Throws Exception.
nx::vector<std::uint16_t> line_glyphs(const nx::string& text, const mbd::Charset& charset) {
    const mbd::Message m = mbd::from_text(text, charset);
    if (m.pages.size() != 1) throw Exception("only characters");
    nx::vector<std::uint16_t> glyphs;
    for (const auto& t : m.pages.front())
        if (t.is_glyph()) glyphs.push_back(t.arg);
        else if (!(t.is_control() && t.control() == mbd::kMessageEnd)) throw Exception("only characters");
    return glyphs;
}

nx::string line_text(std::span<const std::uint16_t> glyphs, const mbd::Charset& charset) {
    nx::string text;
    for (const std::uint16_t g : glyphs) text += charset.glyphs[g];
    return text;
}

// ---- the name table -------------------------------------------------------------------------------------------

// Archive entry 1, loaded at boot: the names of locations, speakers above dialogue and missions, in fields of 104
// bytes: u32 length | 0x2000 << 16, then room for 50 glyph indices (u16; bytes past the length are leftovers).
constexpr std::size_t kNamesEntry = 1, kNameField = 104, kNameGlyphs = 50;
constexpr std::uint16_t kNameFlag = 0x2000;

std::uint16_t read16(std::span<const u8> d, std::size_t at) { return static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8)); }

// Offsets of the fields: a header whose glyphs are all the game's, fields 104 bytes apart.
nx::vector<std::size_t> name_fields(std::span<const u8> file) {
    nx::vector<std::size_t> out;
    for (std::size_t o = 0; o + kNameField <= file.size(); o += 2) {
        const std::uint16_t n = read16(file, o), flag = read16(file, o + 2);
        if (flag != kNameFlag || n == 0 || n > kNameGlyphs) continue;
        bool glyphs = true;
        for (std::size_t i = 0; i < n && glyphs; ++i) glyphs = read16(file, o + 4 + 2 * i) < font::kFirstGlyphs;
        if (!glyphs) continue;
        out.push_back(o);
        o += kNameField - 2;
    }
    return out;
}

int names_export(nx::fs::path_view game, nx::fs::path_view out_path) {
    pkg::Reader reader(pkg::locate(game));
    const auto file = reader.read(kNamesEntry);
    const auto charset = mbd::Charset::european();
    nx::string out = "# Crisis Core -Final Fantasy VII- name table (FF7CCRECOMP cctext names-export): locations, speakers,\n"
                     "# missions. msgctxt: names.<field offset, hex>. At most 50 characters, one line.\n"
                     "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n";
    const auto fields = name_fields(file);
    for (const std::size_t o : fields) {
        nx::string text;
        for (std::size_t i = 0; i < read16(file, o); ++i) text += charset.glyphs[read16(file, o + 4 + 2 * i)];
        out += nx::format("\nmsgctxt \"names.{:05X}\"\nmsgid {}\nmsgstr \"\"\n", o, po_string(text));
    }
    if (!nx::fs::file_write_text(out_path, out)) throw Exception(nx::string("cannot write ") + out_path.native());
    std::fprintf(stderr, "%zu names -> %s\n", fields.size(), nx::string(out_path.native()).c_str());
    return 0;
}

int names_build(nx::fs::path_view game, nx::fs::path_view po_path, nx::fs::path_view mod_dir) {
    pkg::Reader reader(pkg::locate(game));
    nx::vector<u8> file = reader.read(kNamesEntry);
    const auto fields = name_fields(file);
    const auto charset = mod_charset(mod_dir);
    std::size_t changed = 0, errors = 0;
    for (const auto& e : read_po(po_path)) {
        if (e.context.empty() || e.str.empty() || e.str == e.id) continue;
        unsigned offset = 0;
        if (std::sscanf(e.context.c_str(), "names.%x", &offset) != 1 || std::ranges::find(fields, offset) == fields.end())
            throw Exception("PO: " + e.context + " is not a field of the name table");
        try {
            const nx::vector<std::uint16_t> glyphs = line_glyphs(e.str, charset);
            if (glyphs.size() > kNameGlyphs) throw Exception(nx::format("{} characters, at most {}", glyphs.size(), kNameGlyphs));
            std::fill(file.begin() + offset, file.begin() + offset + kNameField, u8{0});
            const auto put = [&](std::size_t at, std::uint16_t v) { file[at] = static_cast<u8>(v), file[at + 1] = static_cast<u8>(v >> 8); };
            put(offset, static_cast<std::uint16_t>(glyphs.size()));
            put(offset + 2, kNameFlag);
            for (std::size_t i = 0; i < glyphs.size(); ++i) put(offset + 4 + 2 * i, glyphs[i]);
            ++changed;
        } catch (const std::exception& x) {
            ++errors;
            std::fprintf(stderr, "%s: %s\n", e.context.c_str(), x.what());
        }
    }
    const nx::string pkg_dir = path_join(mod_dir.native(), "pkg");
    if (!nx::fs::create_directories(pkg_dir)) throw Exception("cannot create directory '" + pkg_dir + "'");
    char name[16];
    std::snprintf(name, sizeof name, "%05zu.bin", kNamesEntry);
    value_or_throw(io::write_file(path_join(pkg_dir, name), file));
    std::fprintf(stderr, "%zu names changed -> %s (%zu errors)\n", changed, path_join(pkg_dir, name).c_str(), errors);
    return errors ? 1 : 0;
}

// ---- menu text ------------------------------------------------------------------------------------------------

// The string tables (accessories, items, materials, materia, commands, abilities: "menus.EEEEE.RRR.name|desc"), the
// field files (missions, mail: "menus.EEEEE.OOOOO", the field's offset in hex) and the inline names of battle modules
// (enemies and actions: "battle.EEEEE.NNN") of ff7cc/formats/strings.hpp.
int menus_export(nx::fs::path_view game, nx::fs::path_view out_path) {
    pkg::Reader reader(pkg::locate(game));
    const auto charset = mbd::Charset::european();
    nx::string out = "# Crisis Core -Final Fantasy VII- menu text (FF7CCRECOMP cctext menus-export): items,\n"
                     "# materia, commands, abilities (menus.<entry>.<record>.name|desc), missions and mail\n"
                     "# (menus.<entry>.<field offset, hex>), battle names (battle.<entry>.<name>); comments give the\n"
                     "# room. One line each.\n"
                     "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n";
    std::size_t count = 0;
    for (const std::size_t entry : strings::kTableEntries) {
        const strings::Table table = strings::parse_table(reader.read(entry));
        for (std::size_t r = 0; r < table.count; ++r)
            for (std::size_t s = 0; s < 2; ++s) {
                if (table.strings[r][s].empty()) continue;
                out += nx::format("\nmsgctxt \"menus.{:05}.{:03}.{}\"\nmsgid {}\nmsgstr \"\"\n", entry, r,
                                  s ? "desc" : "name", po_string(line_text(table.strings[r][s], charset)));
                ++count;
            }
    }
    for (const std::size_t entry : strings::kFieldEntries)
        for (const strings::Field& f : strings::fields(reader.read(entry))) {
            out += nx::format("\n#. at most {} characters\nmsgctxt \"menus.{:05}.{:05X}\"\nmsgid {}\nmsgstr \"\"\n",
                              f.capacity, entry, f.offset, po_string(line_text(f.glyphs, charset)));
            ++count;
        }
    for (std::size_t entry = 0; entry < reader.size(); ++entry) {
        if (reader.sniff(entry) != pkg::FileType::Atel) continue;
        const auto names = strings::inline_names(reader.read(entry));
        for (std::size_t i = 0; i < names.size(); ++i) {
            out += nx::format("\n#. at most {} characters\nmsgctxt \"battle.{:05}.{:03}\"\nmsgid {}\nmsgstr \"\"\n",
                              names[i].capacity, entry, i, po_string(line_text(names[i].glyphs, charset)));
            ++count;
        }
    }
    if (!nx::fs::file_write_text(out_path, out)) throw Exception(nx::string("cannot write ") + out_path.native());
    std::fprintf(stderr, "%zu strings -> %s\n", count, nx::string(out_path.native()).c_str());
    return 0;
}

// String tables are rebuilt from the game's; field files and battle modules patched in the mod's copies when an
// earlier step wrote them (build: mission descriptions and mail end the field files, dialogue is in the modules).
int menus_build(nx::fs::path_view game, nx::fs::path_view po_path, nx::fs::path_view mod_dir) {
    pkg::Reader reader(pkg::locate(game));
    const auto charset = mod_charset(mod_dir);
    nx::map<std::size_t, strings::Table> tables;
    nx::map<std::size_t, std::pair<nx::vector<u8>, nx::vector<strings::Field>>> field_files;
    nx::map<std::size_t, std::pair<nx::vector<u8>, nx::vector<strings::InlineName>>> modules;
    const nx::string pkg_dir = path_join(mod_dir.native(), "pkg");
    std::size_t changed = 0, errors = 0;
    for (const auto& e : read_po(po_path)) {
        if (e.context.empty() || e.str.empty() || e.str == e.id) continue;
        std::size_t entry = 0, record = 0;
        unsigned offset = 0;
        char kind[8] = {};
        try {
            const nx::vector<std::uint16_t> glyphs = line_glyphs(e.str, charset);
            if (std::sscanf(e.context.c_str(), "battle.%zu.%zu", &entry, &record) == 2) {
                auto it = modules.find(entry);
                if (it == modules.end()) {
                    if (entry >= reader.size() || reader.sniff(entry) != pkg::FileType::Atel)
                        throw Exception("not a module");
                    nx::vector<u8> data = mod_or_game(reader, pkg_dir, entry, "atel");
                    nx::vector<strings::InlineName> names = strings::inline_names(data);
                    it = modules.emplace(entry, std::pair(std::move(data), std::move(names))).first;
                }
                const auto& names = it->second.second;
                if (record >= names.size() || line_text(names[record].glyphs, mbd::Charset::european()) != e.id)
                    throw Exception("no such name");
                strings::write_inline(it->second.first, names[record], glyphs);
            } else if (std::sscanf(e.context.c_str(), "menus.%zu.%zu.%7s", &entry, &record, kind) == 3) {
                if (std::ranges::find(strings::kTableEntries, entry) == std::end(strings::kTableEntries))
                    throw Exception("not a string table");
                auto it = tables.find(entry);
                if (it == tables.end()) it = tables.emplace(entry, strings::parse_table(reader.read(entry))).first;
                const std::size_t s = nx::string_view(kind) == "name" ? 0 : nx::string_view(kind) == "desc" ? 1 : 2;
                if (record >= it->second.count || s > 1) throw Exception("no such string");
                it->second.strings[record][s] = glyphs;
            } else if (std::sscanf(e.context.c_str(), "menus.%zu.%x", &entry, &offset) == 2) {
                if (std::ranges::find(strings::kFieldEntries, entry) == std::end(strings::kFieldEntries))
                    throw Exception("not a field file");
                auto it = field_files.find(entry);
                if (it == field_files.end()) {
                    nx::vector<u8> file = mod_or_game(reader, pkg_dir, entry, "bin");
                    nx::vector<strings::Field> found = strings::fields(file);
                    it = field_files.emplace(entry, std::pair(std::move(file), std::move(found))).first;
                }
                const auto& found = it->second.second;
                const auto f = std::ranges::find(found, std::size_t{offset}, &strings::Field::offset);
                if (f == found.end()) throw Exception("no such field");
                strings::write_field(it->second.first, *f, glyphs);
            } else {
                throw Exception("not a menu string");
            }
            ++changed;
        } catch (const std::exception& x) {
            ++errors;
            std::fprintf(stderr, "%s: %s\n", e.context.c_str(), x.what());
        }
    }
    if (!nx::fs::create_directories(pkg_dir)) throw Exception("cannot create directory '" + pkg_dir + "'");
    const auto write = [&](std::size_t entry, std::span<const u8> data, const char* type) {
        value_or_throw(io::write_file(path_join(pkg_dir, nx::format("{:05}.{}", entry, type)), data));
    };
    for (const auto& [entry, table] : tables) write(entry, strings::serialize(table), "bin");
    for (const auto& [entry, file] : field_files) write(entry, file.first, "bin");
    for (const auto& [entry, module] : modules) write(entry, module.first, "atel");
    std::fprintf(stderr, "%zu strings changed in %zu files -> %s (%zu errors)\n", changed,
                 tables.size() + field_files.size() + modules.size(), pkg_dir.c_str(), errors);
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    nx::string game, po, mod_dir, language;
    nx::cli::Parser cli("cctext", "The game's dialogue and menu text as gettext PO files (see docs/localization.md).");
    cli.command("export", "every message; msgctxt \"EEEEE.RR.MMM\" = entry, resource, message")
        .positional("game", game, "the game")
        .positional("out", po, "PO file to write");
    cli.command("build", "modules whose translated text differs -> <mod dir>/pkg/EEEEE.atel")
        .positional("game", game, "the game")
        .positional("po", po, "translated PO file")
        .positional("mod-dir", mod_dir, "mod folder to write")
        .option("language", language, "make the mod a language pack with this name (chosen in the pause menu)");
    cli.command("verify", "binary and text round trips of every message").positional("game", game, "the game");
    nx::string png;
    bool small = false;
    cli.command("font-export", "the regular font's sheet as a picture to draw new characters in")
        .positional("game", game, "the game")
        .positional("out", png, "PNG file to write")
        .flag("small", small, "the small font's sheet (menus) instead");
    cli.command("font-import", "an edited sheet -> <mod dir>/pkg/01472.bin")
        .positional("game", game, "the game")
        .positional("png", png, "the edited sheet")
        .positional("mod-dir", mod_dir, "mod folder to write (its font file, when it has one, gets the sheet)")
        .flag("small", small, "the picture is the small font's sheet (font-export --small)");
    cli.command("names-export", "the name table (locations, speakers, missions); msgctxt \"names.OOOOO\" = field")
        .positional("game", game, "the game")
        .positional("out", po, "PO file to write");
    cli.command("names-build", "the translated name table -> <mod dir>/pkg/00001.bin")
        .positional("game", game, "the game")
        .positional("po", po, "translated PO file")
        .positional("mod-dir", mod_dir, "mod folder to write (its font/glyphs.txt adds characters)");
    cli.command("menus-export", "menu text (items, materia, commands, abilities, missions, mail, battle names)")
        .positional("game", game, "the game")
        .positional("out", po, "PO file to write");
    cli.command("menus-build", "the translated menu text -> <mod dir>/pkg/EEEEE.bin|.atel")
        .positional("game", game, "the game")
        .positional("po", po, "translated PO file")
        .positional("mod-dir", mod_dir, "mod folder to write (its font/glyphs.txt adds characters)");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "export") return export_po(game, po);
        if (command == "build") return build(game, po, mod_dir, language);
        if (command == "verify") return verify(game);
        if (command == "font-export") return font_export(game, png, small);
        if (command == "font-import") return font_import(game, png, mod_dir, small);
        if (command == "names-export") return names_export(game, po);
        if (command == "names-build") return names_build(game, po, mod_dir);
        if (command == "menus-export") return menus_export(game, po);
        if (command == "menus-build") return menus_build(game, po, mod_dir);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctext: %s\n", e.what());
        return 1;
    }
}
