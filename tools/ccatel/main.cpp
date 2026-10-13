// ccatel — inspect, unpack, repack and verify "Atel" script modules.

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/atel_disasm.hpp"
#include "ff7cc/formats/atel_vm.hpp"
#include "ff7cc/io/file.hpp"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

constexpr std::string_view kManifestName = "module.txt";
constexpr std::string_view kManifestHeader = "# ff7cc-atel-manifest 1";

void usage() {
    std::fputs(
        "usage: ccatel <command> [args]\n"
        "\n"
        "  info    <file.atel>               header, objects/methods and resources\n"
        "  strings <file.atel>               every string in the (decrypted) name pool\n"
        "  disasm  <file.atel> [out.txt]     bytecode listing with labels and resolved names\n"
        "  unpack  <file.atel> <dir>         module.txt + names.bin + code.bin + res/\n"
        "  pack    <dir> <file.atel>         rebuild a module from an unpacked directory\n"
        "  verify  <file-or-dir>...          check parse -> serialize and decode -> encode are byte-exact\n",
        stderr);
}

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    for (auto b : bytes) {
        s += kDigits[std::to_integer<unsigned>(b) >> 4];
        s += kDigits[std::to_integer<unsigned>(b) & 15];
    }
    return s;
}

std::vector<std::byte> unhex(std::string_view s) {
    if (s.size() % 2) throw FormatError("odd-length hex string");
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw FormatError("invalid hex digit");
    };
    std::vector<std::byte> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>(nibble(s[2 * i]) << 4 | nibble(s[2 * i + 1]));
    return out;
}

// Names are Shift-JIS; print ASCII as-is and escape everything else so terminals stay sane.
std::string printable(std::string_view s) {
    std::string out;
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

atel::Module load(const fs::path& path) { return atel::parse(io::read_all(path)); }

int cmd_info(const fs::path& path) {
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
        std::printf("  %3zu  %-5s %9zu bytes  head %s\n", i, std::string(atel::resource_type(r.data)).c_str(),
                    r.data.size(), hex(std::span(r.data).first(std::min<std::size_t>(r.data.size(), 8))).c_str());
    }
    return 0;
}

int cmd_strings(const fs::path& path) {
    const auto m = load(path);
    for (std::size_t off = 0; off < m.names.size();) {
        const auto s = m.name_at(static_cast<std::uint32_t>(off));
        std::printf("%6zu  %s\n", off, printable(s).c_str());
        off += s.size() + 1;
    }
    return 0;
}

int cmd_disasm(const fs::path& path, const fs::path& out_path) {
    const auto text = atel::disassemble(load(path));
    if (out_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    } else {
        if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());
        io::write_all(out_path, std::as_bytes(std::span(text)));
    }
    return 0;
}

int cmd_unpack(const fs::path& path, const fs::path& dir) {
    const auto m = load(path);
    fs::create_directories(dir / "res");
    io::write_all(dir / "names.bin", m.names);
    io::write_all(dir / "code.bin", m.code);

    std::ofstream out(dir / kManifestName, std::ios::binary);
    if (!out) throw IoError("cannot write manifest in '" + dir.string() + "'");
    out << kManifestHeader << "\n"
        << "# Name fields are byte offsets into names.bin; method entry points are offsets into code.bin.\n"
        << "# Text after ';' is a comment. Slack/padding hex keeps the module byte-exact.\n";
    char buf[96];
    std::snprintf(buf, sizeof buf, "version %u %u %u 0x%02x\nseed 0x%08x\n", m.version_major, m.version_minor,
                  m.reserved, m.flags, m.seed);
    out << buf << "names names.bin\ncode code.bin\ncode_padding " << hex(m.code_padding) << "\n";
    for (const auto& obj : m.objects) {
        out << "object " << obj.name << " attributes=" << obj.attributes << " unknown=" << obj.unknown << "  ; "
            << printable(m.name_at(obj.name)) << "\n";
        for (const auto& method : obj.methods)
            out << "  method " << method.name << " " << method.code_offset << "  ; "
                << printable(m.name_at(method.name)) << "\n";
    }
    for (std::size_t i = 0; i < m.resources.size(); ++i) {
        const auto& r = m.resources[i];
        std::snprintf(buf, sizeof buf, "res/%03zu.%s", i, std::string(atel::resource_type(r.data)).c_str());
        io::write_all(dir / buf, r.data);
        out << "resource " << buf << " slack=" << hex(r.slack) << "\n";
    }
    if (!out.flush()) throw IoError("error writing manifest");
    std::printf("unpacked %zu objects, %zu resources to %s\n", m.objects.size(), m.resources.size(),
                dir.string().c_str());
    return 0;
}

std::uint32_t parse_u32(std::string_view s) {
    try {
        std::size_t used = 0;
        const unsigned long v = std::stoul(std::string(s), &used, 0);
        if (used != s.size() || v > 0xFFFFFFFFul) throw Error("");
        return static_cast<std::uint32_t>(v);
    } catch (const std::exception&) {
        throw FormatError("invalid number '" + std::string(s) + "'");
    }
}

std::string_view after_prefix(std::string_view token, std::string_view prefix) {
    if (token.substr(0, prefix.size()) != prefix)
        throw FormatError("expected '" + std::string(prefix) + "...', got '" + std::string(token) + "'");
    return token.substr(prefix.size());
}

int cmd_pack(const fs::path& dir, const fs::path& out_path) {
    std::ifstream in(dir / kManifestName, std::ios::binary);
    if (!in) throw IoError("no " + std::string(kManifestName) + " in '" + dir.string() + "'");

    atel::Module m;
    bool header_seen = false;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line == kManifestHeader) header_seen = true;
        if (const auto c = line.find_first_of(";#"); c != std::string::npos) line.resize(c);
        std::istringstream fields(line);
        std::vector<std::string> tok;
        for (std::string t; fields >> t;) tok.push_back(t);
        if (tok.empty()) continue;

        const auto bad = [&] { return FormatError("module.txt line " + std::to_string(line_no) + ": malformed"); };
        const auto& key = tok[0];
        if (key == "version" && tok.size() == 5) {
            m.version_major = static_cast<std::uint8_t>(parse_u32(tok[1]));
            m.version_minor = static_cast<std::uint8_t>(parse_u32(tok[2]));
            m.reserved = static_cast<std::uint8_t>(parse_u32(tok[3]));
            m.flags = static_cast<std::uint8_t>(parse_u32(tok[4]));
        } else if (key == "seed" && tok.size() == 2) {
            m.seed = parse_u32(tok[1]);
        } else if (key == "names" && tok.size() == 2) {
            m.names = io::read_all(dir / tok[1]);
        } else if (key == "code" && tok.size() == 2) {
            m.code = io::read_all(dir / tok[1]);
        } else if (key == "code_padding" && tok.size() <= 2) {
            m.code_padding = tok.size() == 2 ? unhex(tok[1]) : std::vector<std::byte>{};
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
            r.data = io::read_all(dir / tok[1]);
            r.slack = unhex(after_prefix(tok[2], "slack="));
            m.resources.push_back(std::move(r));
        } else {
            throw bad();
        }
    }
    if (!header_seen) throw FormatError("module.txt is missing the '" + std::string(kManifestHeader) + "' header");

    // Round-trip through the parser so a broken module is caught here, not in the game.
    const auto bytes = atel::serialize(m);
    (void)atel::parse(bytes);
    if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());
    io::write_all(out_path, bytes);
    std::printf("packed %s (%zu bytes)\n", out_path.string().c_str(), bytes.size());
    return 0;
}

int cmd_verify(const std::vector<fs::path>& inputs) {
    std::vector<fs::path> files;
    for (const auto& p : inputs) {
        if (fs::is_directory(p)) {
            for (const auto& e : fs::directory_iterator(p))
                if (e.is_regular_file() && e.path().extension() == ".atel") files.push_back(e.path());
        } else {
            files.push_back(p);
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
                std::printf("%s: rebuilt module differs at offset %td (sizes %zu / %zu)\n", f.string().c_str(),
                            diff.first - original.begin(), original.size(), rebuilt.size());
                continue;
            }
            std::vector<std::byte> code;
            for (const auto& insn : atel::decode_all(module.code)) atel::encode(insn, code);
            if (code != module.code) {
                ++failed;
                std::printf("%s: bytecode does not re-encode identically\n", f.string().c_str());
            }
        } catch (const std::exception& e) {
            ++failed;
            std::printf("%s: %s\n", f.string().c_str(), e.what());
        }
    }
    std::printf("%zu modules checked, %zu failed\n", files.size(), failed);
    return failed == 0 ? 0 : 1;
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
        if (cmd == "strings" && args.size() == 2) return cmd_strings(fs::path(args[1]));
        if (cmd == "disasm" && (args.size() == 2 || args.size() == 3))
            return cmd_disasm(fs::path(args[1]), args.size() == 3 ? fs::path(args[2]) : fs::path());
        if (cmd == "unpack" && args.size() == 3) return cmd_unpack(fs::path(args[1]), fs::path(args[2]));
        if (cmd == "pack" && args.size() == 3) return cmd_pack(fs::path(args[1]), fs::path(args[2]));
        if (cmd == "verify" && args.size() >= 2)
            return cmd_verify(std::vector<fs::path>(args.begin() + 1, args.end()));
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccatel: %s\n", e.what());
        return 1;
    }
}
