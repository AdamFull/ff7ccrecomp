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
    for_each_table(reader, [&](std::size_t entry, const atel::Module&, std::size_t r, const nx::vector<mbd::Message>& table) {
        ++tables;
        for (std::size_t m = 0; m < table.size(); ++m) {
            out += "\nmsgctxt \"" + key_string({entry, r, m}) + "\"\nmsgid " + po_string(mbd::to_text(table[m], charset)) +
                   "\nmsgstr \"\"\n";
            ++messages;
        }
    });
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

// ---- the font sheet --------------------------------------------------------------------------------------------

struct FontFile {
    nx::vector<u8> data;
    font::Picture sheet;
    nx::vector<std::uint32_t> palette;
};

FontFile read_font(nx::fs::path_view game) {
    pkg::Reader reader(pkg::locate(game));
    FontFile f;
    f.data = reader.read(font::kFontEntry);
    const auto items = font::items(f.data);
    if (items.size() <= font::kTextPalette) throw Exception("font: unexpected layout");
    f.sheet = font::picture(f.data, items[font::kRegularSheet]);
    f.palette = font::palette(f.data, items[font::kTextPalette]);
    return f;
}

int font_export(nx::fs::path_view game, const nx::string& png) {
    const FontFile f = read_font(game);
    const auto indices = font::unswizzle4(std::span(f.data).subspan(f.sheet.texels, f.sheet.bytes), f.sheet.width,
                                          f.sheet.height);
    nx::vector<std::uint32_t> rgba(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) rgba[i] = f.palette[indices[i]];
    if (const auto written = psprecore::png::write(png, rgba.data(), f.sheet.width, f.sheet.height); !written)
        throw Exception(written.error());
    std::fprintf(stderr, "%dx%d sheet -> %s (cells %dx%d, %d to a row; free: glyphs 96-113, 228-341)\n", f.sheet.width,
                 f.sheet.height, png.c_str(), font::kCellWidth, font::kCellHeight, font::kColumns);
    return 0;
}

// Each pixel becomes the palette colour nearest to it, so new characters should be drawn with the sheet's own
// colours (copy them from existing glyphs).
int font_import(nx::fs::path_view game, const nx::string& png, nx::fs::path_view mod_dir) {
    FontFile f = read_font(game);
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
    cli.command("font-export", "the regular font's sheet as a picture to draw new characters in")
        .positional("game", game, "the game")
        .positional("out", png, "PNG file to write");
    cli.command("font-import", "an edited sheet -> <mod dir>/pkg/01472.bin")
        .positional("game", game, "the game")
        .positional("png", png, "the edited sheet")
        .positional("mod-dir", mod_dir, "mod folder to write");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "export") return export_po(game, po);
        if (command == "build") return build(game, po, mod_dir, language);
        if (command == "verify") return verify(game);
        if (command == "font-export") return font_export(game, png);
        if (command == "font-import") return font_import(game, png, mod_dir);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctext: %s\n", e.what());
        return 1;
    }
}
