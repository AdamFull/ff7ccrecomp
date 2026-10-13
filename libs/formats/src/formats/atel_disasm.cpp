#include "ff7cc/formats/atel_disasm.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "ff7cc/formats/atel_vm.hpp"

namespace ff7cc::atel {

namespace {

std::string hex4(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04X", v);
    return buf;
}

std::string format_float(float f) {
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, f);
    std::string s(buf, ec == std::errc{} ? end : buf);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // keep floats visibly floats
    return s;
}

std::string name_literal(const Module& m, std::int32_t offset) {
    if (offset < 0 || static_cast<std::size_t>(offset) >= m.names.size())
        return "<bad name offset " + std::to_string(offset) + ">";
    return quote(m.name_at(static_cast<std::uint32_t>(offset)));
}

}  // namespace

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out + "\"";
}

std::string disassemble(const Module& m) {
    const auto insns = decode_all(m.code);

    // Labels: method entry points first, then call and jump targets.
    std::map<std::uint32_t, std::vector<std::string>> labels;
    for (const auto& obj : m.objects)
        for (const auto& method : obj.methods)
            labels[method.code_offset].push_back(std::string(m.name_at(obj.name)) + "." +
                                                 std::string(m.name_at(method.name)));
    std::set<std::uint32_t> calls, jumps;
    for (const auto& insn : insns) {
        if (opcode_info(insn.opcode).operand != OperandKind::Code) continue;
        (insn.opcode == 0x28 ? calls : jumps).insert(static_cast<std::uint32_t>(insn.imm));
    }
    // A frame.enter right after an unconditional exit starts a function even if nothing here calls it
    // (e.g. standard-library helpers unused by this module).
    for (std::size_t i = 1; i < insns.size(); ++i) {
        const auto prev = insns[i - 1].opcode;
        if (insns[i].opcode == 0x10 && (prev == 0x2f || prev == 0x01 || prev == 0x30 || prev == 0x27))
            calls.insert(insns[i].offset);
    }
    for (auto target : calls)
        if (!labels.contains(target)) labels[target].push_back("sub_" + hex4(target));
    for (auto target : jumps)
        if (!labels.contains(target)) labels[target].push_back("L_" + hex4(target));
    if (!labels.contains(0)) labels[0].push_back("sub_0000");
    const auto label_of = [&](std::int32_t target) {
        const auto it = labels.find(static_cast<std::uint32_t>(target));
        return it != labels.end() ? it->second.front() : "<bad target " + std::to_string(target) + ">";
    };

    std::string out;
    char buf[64];
    std::snprintf(buf, sizeof buf, "; Atel module, seed 0x%08x\n", m.seed);
    out += buf;
    out += "; " + std::to_string(m.objects.size()) + " objects, " + std::to_string(m.code.size()) +
           " bytes of code, " + std::to_string(insns.size()) + " instructions\n";

    for (const auto& insn : insns) {
        if (const auto it = labels.find(insn.offset); it != labels.end()) {
            out += "\n";
            for (const auto& l : it->second) out += l + ":\n";
        }
        const auto& info = opcode_info(insn.opcode);
        std::string line = "    " + hex4(insn.offset) + "  " + std::string(info.mnemonic);
        std::string operand;
        switch (info.operand) {
            case OperandKind::None: break;
            case OperandKind::I8:
            case OperandKind::I16:
            case OperandKind::I32: operand = std::to_string(insn.imm); break;
            case OperandKind::F32: operand = format_float(insn.fimm); break;
            case OperandKind::Name: operand = name_literal(m, insn.imm); break;
            case OperandKind::Code: operand = label_of(insn.imm); break;
            case OperandKind::NativeCall: {
                operand = name_literal(m, static_cast<std::int32_t>(insn.call.name)) +
                          ", pop=" + std::to_string(insn.call.pop_slots) +
                          (insn.call.has_result ? ", ret" : "");
                if (!insn.call.types.empty()) {
                    operand += ", types=[";
                    for (std::size_t i = 0; i < insn.call.types.size(); ++i) {
                        std::snprintf(buf, sizeof buf, "%s0x%04x", i ? ", " : "", insn.call.types[i]);
                        operand += buf;
                    }
                    operand += "]";
                }
                break;
            }
        }
        if (!operand.empty()) {
            line.resize(std::max<std::size_t>(line.size() + 1, 26), ' ');
            line += operand;
        }
        out += line + "\n";
    }
    return out;
}

}  // namespace ff7cc::atel
