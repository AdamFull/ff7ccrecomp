// cctext — the game's dialogue and menu text (MBD tables in Atel modules) as gettext PO files.
//
//   cctext export <game> <out.po>             every message; msgctxt "EEEEE.RR.MMM" = entry, resource, message
//   cctext build  <game> <in.po> <mod dir>    modules whose translated text differs -> <mod dir>/pkg/EEEEE.atel
//   cctext verify <game>                      binary and text round trips of every message
//
// See docs/localization.md for the text syntax ({PAGE}, {CHOICE:200}, {C10}, {G:84}).

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/mbd.hpp"
#include "ff7cc/formats/pkg.hpp"
#include "ff7cc/io/file.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

void usage() {
    std::fputs(
        "usage: cctext export <game> <out.po>\n"
        "       cctext build  <game> <in.po> <mod dir>\n"
        "       cctext verify <game>\n",
        stderr);
}

struct Key {
    std::size_t entry = 0, resource = 0, message = 0;
    auto operator<=>(const Key&) const = default;
};

std::string key_string(const Key& k) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%05zu.%02zu.%03zu", k.entry, k.resource, k.message);
    return buf;
}

std::optional<Key> parse_key(std::string_view s) {
    Key k;
    if (std::sscanf(std::string(s).c_str(), "%zu.%zu.%zu", &k.entry, &k.resource, &k.message) != 3) return std::nullopt;
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

std::string po_escape(std::string_view s) {
    std::string out;
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
std::string po_string(std::string_view s) {
    if (s.find('\n') == std::string_view::npos || s.find('\n') == s.size() - 1) return "\"" + po_escape(s) + "\"";
    std::string out = "\"\"";
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t nl = s.find('\n', start);
        const std::size_t end = nl == std::string_view::npos ? s.size() : nl + 1;
        out += "\n\"" + po_escape(s.substr(start, end - start)) + "\"";
        start = end;
    }
    return out;
}

std::string po_unescape(std::string_view s) {
    std::string out;
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
    std::string context, id, str;
};

nx::vector<PoEntry> read_po(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("cannot read " + path.string());
    nx::vector<PoEntry> out;
    PoEntry cur;
    std::string* field = nullptr;
    const auto flush = [&] {
        if (!cur.context.empty() || !cur.id.empty()) out.push_back(cur);
        cur = PoEntry{};
        field = nullptr;
    };
    const auto quoted = [](std::string_view line) {
        const auto a = line.find('"'), b = line.rfind('"');
        if (a == std::string_view::npos || b <= a) throw FormatError("PO: expected a quoted string: " + std::string(line));
        return po_unescape(line.substr(a + 1, b - a - 1));
    };
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
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

int export_po(const fs::path& game, const fs::path& out_path) {
    pkg::Reader reader(pkg::locate(game));
    const auto charset = mbd::Charset::european();
    std::ofstream out(out_path, std::ios::binary);
    if (!out) throw IoError("cannot write " + out_path.string());
    out << "# Crisis Core -Final Fantasy VII- text (FF7CCRECOMP cctext). msgctxt: archive entry.resource.message\n"
           "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n";
    std::size_t messages = 0, tables = 0;
    for_each_table(reader, [&](std::size_t entry, const atel::Module&, std::size_t r, const nx::vector<mbd::Message>& table) {
        ++tables;
        for (std::size_t m = 0; m < table.size(); ++m) {
            out << "\nmsgctxt \"" << key_string({entry, r, m}) << "\"\nmsgid " << po_string(mbd::to_text(table[m], charset))
                << "\nmsgstr \"\"\n";
            ++messages;
        }
    });
    std::fprintf(stderr, "%zu messages from %zu tables -> %s\n", messages, tables, out_path.string().c_str());
    return 0;
}

int verify(const fs::path& game) {
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
            const std::string text = mbd::to_text(table[m], charset);
            if (text.find("{G:") != std::string::npos) ++unnamed;
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

int build(const fs::path& game, const fs::path& po_path, const fs::path& mod_dir) {
    const auto charset = mbd::Charset::european();
    nx::map<Key, std::string> translated;
    for (const auto& e : read_po(po_path)) {
        if (e.context.empty() || e.str.empty() || e.str == e.id) continue;
        const auto key = parse_key(e.context);
        if (!key) throw FormatError("PO: bad msgctxt " + e.context);
        translated[*key] = e.str;
    }
    pkg::Reader reader(pkg::locate(game));
    nx::map<std::size_t, nx::vector<std::pair<Key, std::string>>> by_entry;
    for (const auto& [k, s] : translated) by_entry[k.entry].emplace_back(k, s);

    fs::create_directories(mod_dir / "pkg");
    std::size_t modules = 0, changed = 0, errors = 0;
    for (const auto& [entry, items] : by_entry) {
        if (entry >= reader.size() || reader.sniff(entry) != pkg::FileType::Atel)
            throw FormatError("PO: entry " + std::to_string(entry) + " is not a script module");
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
        io::write_all(mod_dir / "pkg" / name, atel::replace_resources(data, replacements));
        ++modules;
    }
    std::fprintf(stderr, "%zu messages changed in %zu modules -> %s (%zu errors)\n", changed, modules,
                 (mod_dir / "pkg").string().c_str(), errors);
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view cmd = argc > 1 ? argv[1] : "";
        if (cmd == "export" && argc == 4) return export_po(argv[2], argv[3]);
        if (cmd == "build" && argc == 5) return build(argv[2], argv[3], argv[4]);
        if (cmd == "verify" && argc == 3) return verify(argv[2]);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cctext: %s\n", e.what());
        return 1;
    }
}
