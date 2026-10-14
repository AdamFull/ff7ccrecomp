// cctext — the game's dialogue and menu text (MBD tables in Atel modules) as gettext PO files.
//
//   cctext export <game> <out.po>             every message; msgctxt "EEEEE.RR.MMM" = entry, resource, message
//   cctext build  <game> <in.po> <mod dir>    modules whose translated text differs -> <mod dir>/pkg/EEEEE.atel
//   cctext verify <game>                      binary and text round trips of every message
//
// See docs/localization.md for the text syntax ({PAGE}, {CHOICE:200}, {C10}, {G:150}).

#include <cstdio>
#include <exception>
#include <optional>

#include "psprecore/error.hpp"
#include "psprecore/cli.hpp"
#include "ff7cc/formats/atel.hpp"
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
    if (!file) throw IoError(nx::string("cannot read ") + path.native());
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
            throw FormatError(nx::string("PO: expected a quoted string: ") + line);
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
    if (!nx::fs::file_write_text(out_path, out)) throw IoError(nx::string("cannot write ") + out_path.native());
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

int build(nx::fs::path_view game, nx::fs::path_view po_path, nx::fs::path_view mod_dir) {
    const auto charset = mbd::Charset::european();
    nx::map<Key, nx::string> translated;
    for (const auto& e : read_po(po_path)) {
        if (e.context.empty() || e.str.empty() || e.str == e.id) continue;
        const auto key = parse_key(e.context);
        if (!key) throw FormatError("PO: bad msgctxt " + e.context);
        translated[*key] = e.str;
    }
    pkg::Reader reader(pkg::locate(game));
    nx::map<std::size_t, nx::vector<std::pair<Key, nx::string>>> by_entry;
    for (const auto& [k, s] : translated) by_entry[k.entry].emplace_back(k, s);

    const nx::string pkg_dir = path_join(mod_dir.native(), "pkg");
    if (!nx::fs::create_directories(pkg_dir)) throw IoError("cannot create directory '" + pkg_dir + "'");
    std::size_t modules = 0, changed = 0, errors = 0;
    for (const auto& [entry, items] : by_entry) {
        if (entry >= reader.size() || reader.sniff(entry) != pkg::FileType::Atel)
            throw FormatError("PO: entry " + nx::format("{}", entry) + " is not a script module");
        const auto data = reader.read(entry);
        atel::Module module = atel::parse(data);
        // Scripts address messages and resources by offset: changed messages are patched in place (mbd::patch) and
        // the tables replaced without moving other resources (atel::replace_resources).
        nx::map<std::size_t, nx::map<std::size_t, mbd::Message>> changes;  // resource -> message -> content
        for (const auto& [k, text] : items) {
            if (k.resource >= module.resources.size() || !mbd::is_mbd(module.resources[k.resource].data))
                throw FormatError("PO: " + key_string(k) + " is not a text table");
            const auto count = mbd::parse(module.resources[k.resource].data).size();
            if (k.message >= count) throw FormatError("PO: " + key_string(k) + " is past the end of its table");
            try {
                changes[k.resource][k.message] = mbd::from_text(text, charset);
                ++changed;
            } catch (const FormatError& e) {
                ++errors;
                std::fprintf(stderr, "%s: %s\n", key_string(k).c_str(), e.what());
            }
        }
        nx::map<std::size_t, nx::vector<std::byte>> replacements;
        for (const auto& [r, messages] : changes) replacements[r] = mbd::patch(module.resources[r].data, messages);
        char name[16];
        std::snprintf(name, sizeof name, "%05zu.atel", entry);
        io::write_all(path_join(pkg_dir, name), atel::replace_resources(data, replacements));
        ++modules;
    }
    std::fprintf(stderr, "%zu messages changed in %zu modules -> %s (%zu errors)\n", changed, modules,
                 pkg_dir.c_str(), errors);
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    nx::string game, po, mod_dir;
    nx::cli::Parser cli("cctext", "The game's dialogue and menu text as gettext PO files (see docs/localization.md).");
    cli.command("export", "every message; msgctxt \"EEEEE.RR.MMM\" = entry, resource, message")
        .positional("game", game, "the game")
        .positional("out", po, "PO file to write");
    cli.command("build", "modules whose translated text differs -> <mod dir>/pkg/EEEEE.atel")
        .positional("game", game, "the game")
        .positional("po", po, "translated PO file")
        .positional("mod-dir", mod_dir, "mod folder to write");
    cli.command("verify", "binary and text round trips of every message").positional("game", game, "the game");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "export") return export_po(game, po);
        if (command == "build") return build(game, po, mod_dir);
        if (command == "verify") return verify(game);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctext: %s\n", e.what());
        return 1;
    }
}
