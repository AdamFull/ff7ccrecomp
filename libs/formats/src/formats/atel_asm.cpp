#include "ff7cc/formats/atel_asm.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <charconv>
#include <cstdlib>
#include <string>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atel_vm.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace ff7cc::atel {

namespace {

struct Line {
    std::size_t number;
    nx::vector<std::string> tokens;  // string literals keep their quotes; brackets are whole tokens
};

[[noreturn]] void fail(std::size_t line, const std::string& what) {
    throw FormatError("assembly line " + std::to_string(line) + ": " + what);
}

// Splits on whitespace and commas, keeping "..." literals (with escapes) and [...] lists intact.
nx::vector<std::string> tokenize(std::string_view text, std::size_t line) {
    nx::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ';') break;
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            ++i;
            continue;
        }
        std::size_t j = i;
        if (c == '"') {
            for (++j; j < text.size() && text[j] != '"'; ++j)
                if (text[j] == '\\') ++j;
            if (j >= text.size()) fail(line, "unterminated string literal");
            ++j;
        } else if (c == '[') {
            j = text.find(']', i);
            if (j == std::string_view::npos) fail(line, "unterminated '['");
            ++j;
        } else {
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != ',' &&
                   text[j] != ';') {
                if (text[j] == '[') {  // e.g. types=[0x0001, 0x0000]: keep the list in this token
                    j = text.find(']', j);
                    if (j == std::string_view::npos) fail(line, "unterminated '['");
                }
                ++j;
            }
        }
        out.emplace_back(text.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string unquote(std::string_view tok, std::size_t line) {
    if (tok.size() < 2 || tok.front() != '"' || tok.back() != '"') fail(line, "expected a string literal");
    std::string out;
    for (std::size_t i = 1; i + 1 < tok.size(); ++i) {
        if (tok[i] != '\\') {
            out += tok[i];
            continue;
        }
        if (++i + 1 >= tok.size()) fail(line, "bad escape");
        if (tok[i] == 'x') {
            if (i + 2 >= tok.size()) fail(line, "bad \\x escape");
            unsigned v = 0;
            const auto r = std::from_chars(tok.data() + i + 1, tok.data() + i + 3, v, 16);
            if (r.ec != std::errc{} || r.ptr != tok.data() + i + 3) fail(line, "bad \\x escape");
            out += static_cast<char>(v);
            i += 2;
        } else {
            out += tok[i];  // \" and \\ (and any other escaped character literally)
        }
    }
    if (out.find('\0') != std::string::npos) fail(line, "string literal contains NUL");
    return out;
}

std::int64_t parse_int(std::string_view tok, std::size_t line) {
    bool negative = false;
    std::string_view s = tok;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        negative = s[0] == '-';
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    std::uint64_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) fail(line, "invalid integer '" + std::string(tok) + "'");
    return negative ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
}

std::int32_t parse_ranged(std::string_view tok, std::int64_t lo, std::int64_t hi, std::size_t line) {
    const auto v = parse_int(tok, line);
    if (v < lo || v > hi) fail(line, "value '" + std::string(tok) + "' out of range");
    return static_cast<std::int32_t>(v);
}

float parse_float(std::string_view tok, std::size_t line) {
    if (tok.starts_with("bits(") && tok.ends_with(")"))
        return std::bit_cast<float>(static_cast<std::uint32_t>(
            parse_ranged(tok.substr(5, tok.size() - 6), 0, 0xFFFFFFFFll, line)));
    const std::string s(tok);
    char* end = nullptr;
    const float f = std::strtof(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size()) fail(line, "invalid float '" + s + "'");
    return f;
}

bool is_hex_column(std::string_view tok) {
    return !tok.empty() && std::all_of(tok.begin(), tok.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

class Pool {
public:
    explicit Pool(nx::vector<std::byte>& names) : names_(names) {
        for (std::size_t off = 0; off < names_.size();) {
            const auto* p = reinterpret_cast<const char*>(names_.data()) + off;
            const auto len = std::find(p, p + (names_.size() - off), '\0') - p;
            first_.try_emplace(std::string(p, static_cast<std::size_t>(len)), static_cast<std::uint32_t>(off));
            off += static_cast<std::size_t>(len) + 1;
        }
    }

    std::uint32_t intern(const std::string& s) {
        if (const auto it = first_.find(s); it != first_.end()) return it->second;
        const auto off = static_cast<std::uint32_t>(names_.size());
        for (char c : s) names_.push_back(static_cast<std::byte>(c));
        names_.push_back(std::byte{0});
        first_.emplace(s, off);
        return off;
    }

private:
    nx::vector<std::byte>& names_;
    nx::map<std::string, std::uint32_t> first_;
};

}  // namespace

void assemble_into(Module& module, std::string_view listing) {
    // Parse lines.
    nx::vector<Line> lines;
    std::size_t number = 0;
    for (std::size_t pos = 0; pos <= listing.size();) {
        const auto nl = listing.find('\n', pos);
        auto text = listing.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        ++number;
        pos = nl == std::string_view::npos ? listing.size() + 1 : nl + 1;
        auto tokens = tokenize(text, number);
        if (!tokens.empty()) lines.push_back(Line{number, std::move(tokens)});
    }

    auto names = module.names;
    Pool pool(names);

    // Pass 1: build instructions with placeholder code targets; record label offsets.
    struct Pending {
        Instruction insn;
        std::string target;  // label for Code operands
        std::size_t line;
    };
    nx::vector<Pending> program;
    nx::map<std::string, std::uint32_t> labels;
    std::uint32_t offset = 0;

    for (const auto& [line, tok] : lines) {
        if (tok.size() == 1 && tok[0].size() > 1 && tok[0].back() == ':') {
            const auto name = tok[0].substr(0, tok[0].size() - 1);
            if (!labels.emplace(name, offset).second && labels[name] != offset) fail(line, "duplicate label '" + name + "'");
            continue;
        }
        std::size_t t = 0;
        if (tok.size() >= 2 && is_hex_column(tok[0]) && opcode_from_mnemonic(tok[1])) t = 1;  // skip offset column
        const auto op = opcode_from_mnemonic(tok[t]);
        if (!op) fail(line, "unknown mnemonic '" + tok[t] + "'");
        const auto args = nx::vector<std::string>(tok.begin() + static_cast<std::ptrdiff_t>(t) + 1, tok.end());

        Pending p{Instruction{}, {}, line};
        p.insn.opcode = *op;
        const auto kind = opcode_info(*op).operand;
        const auto want = [&](std::size_t n) {
            if (args.size() != n) fail(line, std::string(opcode_info(*op).mnemonic) + " expects " + std::to_string(n) + " operand(s)");
        };
        switch (kind) {
            case OperandKind::None: want(0); break;
            case OperandKind::I8: want(1); p.insn.imm = parse_ranged(args[0], -128, 255, line); break;
            case OperandKind::I16: want(1); p.insn.imm = parse_ranged(args[0], -32768, 65535, line); break;
            case OperandKind::I32: want(1); p.insn.imm = parse_ranged(args[0], INT32_MIN, UINT32_MAX, line); break;
            case OperandKind::F32: want(1); p.insn.fimm = parse_float(args[0], line); break;
            case OperandKind::Name: want(1); p.insn.imm = static_cast<std::int32_t>(pool.intern(unquote(args[0], line))); break;
            case OperandKind::Code: want(1); p.target = args[0]; break;
            case OperandKind::NativeCall: {
                if (args.empty()) fail(line, "calln expects a function name");
                p.insn.call.name = pool.intern(unquote(args[0], line));
                for (std::size_t i = 1; i < args.size(); ++i) {
                    const std::string_view a = args[i];
                    if (a == "ret") {
                        p.insn.call.has_result = 1;
                    } else if (a.starts_with("pop=")) {
                        p.insn.call.pop_slots = static_cast<std::int16_t>(parse_ranged(a.substr(4), -32768, 32767, line));
                    } else if (a.starts_with("types=")) {
                        const auto list = a.substr(6);
                        if (list.size() < 2 || list.front() != '[' || list.back() != ']') fail(line, "types must be [..]");
                        for (const auto& item : tokenize(list.substr(1, list.size() - 2), line))
                            p.insn.call.types.push_back(static_cast<std::uint16_t>(parse_ranged(item, 0, 0xFFFF, line)));
                    } else {
                        fail(line, "unknown calln argument '" + std::string(a) + "'");
                    }
                }
                break;
            }
        }
        nx::vector<std::byte> scratch;
        encode(p.insn, scratch);
        p.insn.offset = offset;
        offset += static_cast<std::uint32_t>(scratch.size());
        program.push_back(std::move(p));
    }

    // Pass 2: resolve jump/call targets and encode.
    nx::vector<std::byte> code;
    for (auto& p : program) {
        if (!p.target.empty()) {
            const auto it = labels.find(p.target);
            if (it == labels.end()) fail(p.line, "undefined label '" + p.target + "'");
            p.insn.imm = static_cast<std::int32_t>(it->second);
        }
        encode(p.insn, code);
    }

    // Method entry points from labels.
    auto objects = module.objects;
    for (auto& obj : objects) {
        for (auto& method : obj.methods) {
            const auto label = std::string(module.name_at(obj.name)) + "." + std::string(module.name_at(method.name));
            const auto it = labels.find(label);
            if (it == labels.end()) throw FormatError("assembly has no label for method '" + label + "'");
            if (it->second >= code.size()) throw FormatError("method label '" + label + "' is past the end of the code");
            method.code_offset = it->second;
        }
    }

    module.names = std::move(names);
    module.code = std::move(code);
    module.objects = std::move(objects);
}

}  // namespace ff7cc::atel
