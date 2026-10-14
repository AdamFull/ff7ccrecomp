// ccatel — inspect, unpack, repack and verify "Atel" script modules.

#include <algorithm>
#include <cstdio>
#include <exception>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/atel_asm.hpp"
#include "ff7cc/formats/atel_disasm.hpp"
#include "ff7cc/formats/atel_vm.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/io/path.hpp"
#include "ff7cc/text.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;

namespace {

constexpr nx::string_view kManifestName = "module.txt";
constexpr nx::string_view kManifestHeader = "# ff7cc-atel-manifest 1";

void usage() {
    std::fputs(
        "usage: ccatel <command> [args]\n"
        "\n"
        "  info    <file.atel>               header, objects/methods and resources\n"
        "  strings <file.atel>               every string in the (decrypted) name pool\n"
        "  disasm  <file.atel> [out.txt]     bytecode listing with labels and resolved names\n"
        "  unpack  <file.atel> <dir>         module.txt + names.bin + code.asm (editable) + res/\n"
        "  pack    <dir> <file.atel>         rebuild a module from an unpacked directory\n"
        "  verify  <file-or-dir>...          check container, bytecode and listing round trips are byte-exact\n",
        stderr);
}

nx::string hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    nx::string s;
    for (auto b : bytes) {
        s += kDigits[std::to_integer<unsigned>(b) >> 4];
        s += kDigits[std::to_integer<unsigned>(b) & 15];
    }
    return s;
}

nx::vector<std::byte> unhex(nx::string_view s) {
    if (s.size() % 2) throw FormatError("odd-length hex string");
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw FormatError("invalid hex digit");
    };
    nx::vector<std::byte> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>(nibble(s[2 * i]) << 4 | nibble(s[2 * i + 1]));
    return out;
}

// Names are Shift-JIS; print ASCII as-is and escape everything else so terminals stay sane.
nx::string printable(nx::string_view s) {
    nx::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

std::span<const std::byte> text_bytes(nx::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }

void make_parent_directories(nx::fs::path_view path) {
    const nx::fs::path_view parent = path.parent_path();
    if (!parent.empty() && !nx::fs::create_directories(parent))
        throw IoError(nx::string("cannot create directory '") + parent.native() + "'");
}

atel::Module load(nx::fs::path_view path) { return atel::parse(io::read_all(path)); }

int cmd_info(nx::fs::path_view path) {
    const auto m = load(path);
    std::printf("version %u.%u  flags 0x%02x  seed 0x%08x\n", m.version_major, m.version_minor, m.flags, m.seed);
    std::printf("names %zu bytes, code %zu bytes, %zu objects, %zu resources\n\n", m.names.size(), m.code.size(),
                m.objects.size(), m.resources.size());
    for (const auto& obj : m.objects) {
        std::printf("object %s  (attributes %u, unknown %u)\n", printable(m.name_at(obj.name)).c_str(), obj.attributes,
                    obj.unknown);
        for (const auto& method : obj.methods)
            std::printf("  %-28s @ %u\n", printable(m.name_at(method.name)).c_str(), method.code_offset);
    }
    std::puts("\nresources:");
    for (std::size_t i = 0; i < m.resources.size(); ++i) {
        const auto& r = m.resources[i];
        std::printf("  %3zu  %-5s %9zu bytes  head %s\n", i, nx::string(atel::resource_type(r.data)).c_str(),
                    r.data.size(), hex(std::span(r.data).first(std::min<std::size_t>(r.data.size(), 8))).c_str());
    }
    return 0;
}

int cmd_strings(nx::fs::path_view path) {
    const auto m = load(path);
    for (std::size_t off = 0; off < m.names.size();) {
        const auto s = m.name_at(static_cast<std::uint32_t>(off));
        std::printf("%6zu  %s\n", off, printable(s).c_str());
        off += s.size() + 1;
    }
    return 0;
}

int cmd_disasm(nx::fs::path_view path, nx::fs::path_view out_path) {
    const auto text = atel::disassemble(load(path));
    if (out_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    } else {
        make_parent_directories(out_path);
        io::write_all(out_path, text_bytes(text));
    }
    return 0;
}

int cmd_unpack(nx::fs::path_view path, nx::fs::path_view dir) {
    const auto m = load(path);
    const nx::string res_dir = path_join(dir.native(), "res");
    if (!nx::fs::create_directories(res_dir)) throw IoError("cannot create directory '" + res_dir + "'");
    io::write_all(path_join(dir.native(), "names.bin"), m.names);
    const auto listing = atel::disassemble(m);
    io::write_all(path_join(dir.native(), "code.asm"), text_bytes(listing));

    nx::string out(kManifestHeader);
    out += "\n"
           "# Name fields are byte offsets into names.bin. With code.asm, method entry points come from its\n"
           "# Object.method labels and new string literals are appended to names.bin when packing.\n"
           "# Text after ';' is a comment. Slack/padding hex keeps the module byte-exact.\n";
    char buf[96];
    std::snprintf(buf, sizeof buf, "version %u %u %u 0x%02x\nseed 0x%08x\n", m.version_major, m.version_minor,
                  m.reserved, m.flags, m.seed);
    out += buf;
    out += "names names.bin\ncode code.asm\ncode_padding " + hex(m.code_padding) + "\n";
    for (const auto& obj : m.objects) {
        out += nx::format("object {} attributes={} unknown={}  ; {}\n", obj.name, obj.attributes, obj.unknown,
                          printable(m.name_at(obj.name)));
        for (const auto& method : obj.methods)
            out += nx::format("  method {} {}  ; {}\n", method.name, method.code_offset,
                              printable(m.name_at(method.name)));
    }
    for (std::size_t i = 0; i < m.resources.size(); ++i) {
        const auto& r = m.resources[i];
        std::snprintf(buf, sizeof buf, "res/%03zu.%s", i, nx::string(atel::resource_type(r.data)).c_str());
        io::write_all(path_join(dir.native(), buf), r.data);
        out += nx::format("resource {} slack={}\n", static_cast<const char*>(buf), hex(r.slack));
    }
    if (!nx::fs::file_write_text(path_join(dir.native(), kManifestName), out))
        throw IoError(nx::string("cannot write manifest in '") + dir.native() + "'");
    std::printf("unpacked %zu objects, %zu resources to %s\n", m.objects.size(), m.resources.size(),
                nx::string(dir.native()).c_str());
    return 0;
}

// Decimal or 0x-prefixed hex.
std::uint32_t parse_u32(nx::string_view s) {
    try {
        const std::uint64_t v = parse_u64(s, 0);
        if (v > 0xFFFFFFFFu) throw Error("");
        return static_cast<std::uint32_t>(v);
    } catch (const std::exception&) {
        throw FormatError(nx::string("invalid number '") + s + "'");
    }
}

nx::string_view after_prefix(nx::string_view token, nx::string_view prefix) {
    if (token.substr(0, prefix.size()) != prefix)
        throw FormatError(nx::string("expected '") + prefix + "...', got '" + token + "'");
    return token.substr(prefix.size());
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

int cmd_pack(nx::fs::path_view dir, nx::fs::path_view out_path) {
    const auto manifest = nx::fs::file_read_text(path_join(dir.native(), kManifestName));
    if (!manifest) throw IoError(nx::string("no ") + kManifestName + " in '" + dir.native() + "'");

    atel::Module m;
    nx::string code_path;
    bool header_seen = false;
    const nx::string_view text = manifest.value();
    std::size_t line_no = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == nx::string_view::npos ? text.size() : nl;
        nx::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line == kManifestHeader) header_seen = true;
        if (const auto c = line.find_first_of(";#"); c != nx::string_view::npos) line = line.substr(0, c);
        nx::vector<nx::string_view> tok;
        for (std::size_t i = 0; i < line.size();) {
            if (is_space(line[i])) {
                ++i;
                continue;
            }
            const std::size_t start = i;
            while (i < line.size() && !is_space(line[i])) ++i;
            tok.push_back(line.substr(start, i - start));
        }
        if (tok.empty()) continue;

        const auto bad = [&] { return FormatError("module.txt line " + nx::format("{}", line_no) + ": malformed"); };
        const auto key = tok[0];
        if (key == "version" && tok.size() == 5) {
            m.version_major = static_cast<std::uint8_t>(parse_u32(tok[1]));
            m.version_minor = static_cast<std::uint8_t>(parse_u32(tok[2]));
            m.reserved = static_cast<std::uint8_t>(parse_u32(tok[3]));
            m.flags = static_cast<std::uint8_t>(parse_u32(tok[4]));
        } else if (key == "seed" && tok.size() == 2) {
            m.seed = parse_u32(tok[1]);
        } else if (key == "names" && tok.size() == 2) {
            m.names = io::read_all(path_join(dir.native(), tok[1]));
        } else if (key == "code" && tok.size() == 2) {
            code_path = path_join(dir.native(), tok[1]);
        } else if (key == "code_padding" && tok.size() <= 2) {
            m.code_padding = tok.size() == 2 ? unhex(tok[1]) : nx::vector<std::byte>{};
        } else if (key == "object" && tok.size() == 4) {
            atel::Object obj;
            obj.name = parse_u32(tok[1]);
            obj.attributes = static_cast<std::uint16_t>(parse_u32(after_prefix(tok[2], "attributes=")));
            obj.unknown = parse_u32(after_prefix(tok[3], "unknown="));
            m.objects.push_back(std::move(obj));
        } else if (key == "method" && tok.size() == 3) {
            if (m.objects.empty()) throw bad();
            m.objects.back().methods.push_back(atel::Method{parse_u32(tok[1]), parse_u32(tok[2])});
        } else if (key == "resource" && tok.size() == 3) {
            atel::Resource r;
            r.data = io::read_all(path_join(dir.native(), tok[1]));
            r.slack = unhex(after_prefix(tok[2], "slack="));
            m.resources.push_back(std::move(r));
        } else {
            throw bad();
        }
    }
    if (!header_seen) throw FormatError(nx::string("module.txt is missing the '") + kManifestHeader + "' header");
    if (code_path.empty()) throw FormatError("module.txt has no 'code' line");
    // Code is assembled last: it needs the objects (for method labels) and may append to the name pool.
    if (nx::fs::path_view(code_path).extension() == ".asm") {
        const auto code_text = io::read_all(code_path);
        atel::assemble_into(m, nx::string_view(reinterpret_cast<const char*>(code_text.data()), code_text.size()));
    } else {
        m.code = io::read_all(code_path);
    }

    // Round-trip through the parser so a broken module is caught here, not in the game.
    const auto bytes = atel::serialize(m);
    (void)atel::parse(bytes);
    make_parent_directories(out_path);
    io::write_all(out_path, bytes);
    std::printf("packed %s (%zu bytes)\n", nx::string(out_path.native()).c_str(), bytes.size());
    return 0;
}

int cmd_verify(const nx::vector<nx::string_view>& inputs) {
    nx::vector<nx::string> files;
    for (const auto p : inputs) {
        if (is_directory(p)) {
            const auto entries = nx::fs::directory_entries(p);
            if (!entries) throw IoError(nx::string("cannot list directory '") + p + "'");
            for (const auto& e : entries.value())
                if (e.type == nx::fs::file_type::regular && nx::fs::path_view(e.path).extension() == ".atel")
                    files.push_back(e.path);
        } else {
            files.emplace_back(p);
        }
    }
    std::sort(files.begin(), files.end());

    std::size_t failed = 0;
    for (const auto& f : files) {
        try {
            const auto original = io::read_all(f);
            const auto module = atel::parse(original);
            const auto rebuilt = atel::serialize(module);
            if (rebuilt != original) {
                ++failed;
                const auto diff = std::mismatch(original.begin(), original.end(), rebuilt.begin(), rebuilt.end());
                std::printf("%s: rebuilt module differs at offset %td (sizes %zu / %zu)\n", f.c_str(),
                            diff.first - original.begin(), original.size(), rebuilt.size());
                continue;
            }
            nx::vector<std::byte> code;
            for (const auto& insn : atel::decode_all(module.code)) atel::encode(insn, code);
            if (code != module.code) {
                ++failed;
                std::printf("%s: bytecode does not re-encode identically\n", f.c_str());
                continue;
            }
            auto reassembled = module;
            atel::assemble_into(reassembled, atel::disassemble(module));
            if (atel::serialize(reassembled) != original) {
                ++failed;
                std::printf("%s: disassemble -> assemble does not reproduce the module\n", f.c_str());
            }
        } catch (const std::exception& e) {
            ++failed;
            std::printf("%s: %s\n", f.c_str(), e.what());
        }
    }
    std::printf("%zu modules checked, %zu failed\n", files.size(), failed);
    return failed == 0 ? 0 : 1;
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
        if (cmd == "strings" && args.size() == 2) return cmd_strings(args[1]);
        if (cmd == "disasm" && (args.size() == 2 || args.size() == 3))
            return cmd_disasm(args[1], args.size() == 3 ? args[2] : nx::string_view());
        if (cmd == "unpack" && args.size() == 3) return cmd_unpack(args[1], args[2]);
        if (cmd == "pack" && args.size() == 3) return cmd_pack(args[1], args[2]);
        if (cmd == "verify" && args.size() >= 2)
            return cmd_verify(nx::vector<nx::string_view>(args.begin() + 1, args.end()));
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccatel: %s\n", e.what());
        return 1;
    }
}
