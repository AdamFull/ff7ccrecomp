#include "ff7cc/formats/atel_vm.hpp"

#include <bit>

#include "psprecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

namespace ff7cc::atel {

namespace {

using K = OperandKind;

// clang-format off
constexpr std::array<OpcodeInfo, kOpcodeCount> kOpcodes{{
    /* 00 */ {"reset"},               // reset the thread (pc = 0, clear stacks)
    /* 01 */ {"halt"},                // stop the thread
    /* 02 */ {"suspend"},             // ? sets frame flag 146 (also set by waiting requests)
    /* 03 */ {"flag147.set"},         // ? frame flag 147
    /* 04 */ {"flag147.clr"},         // ? frame flag 147
    /* 05 */ {"push.0"},
    /* 06 */ {"push.0f"},
    /* 07 */ {"push.i8", K::I8},
    /* 08 */ {"push.i16", K::I16},
    /* 09 */ {"push.i32", K::I32},
    /* 0a */ {"push.f32", K::F32},
    /* 0b */ {"push.str", K::Name},   // pointer to a name-pool string
    /* 0c */ {"push.level"},          // current priority level
    /* 0d */ {"push.global", K::Name},// value of a module global looked up by name (ELF hash)
    /* 0e */ {"push.self"},           // this thread/object
    /* 0f */ {"push.frameobj"},       // object stored in the current frame (+116)
    /* 10 */ {"frame.enter", K::I32}, // reserve n bytes on the locals stack
    /* 11 */ {"frame.leave", K::I32}, // release n bytes of locals
    /* 12 */ {"dup"},
    /* 13 */ {"dup.n", K::I32},       // duplicate the top n bytes
    /* 14 */ {"pop"},
    /* 15 */ {"pop.n", K::I32},       // drop n bytes
    /* 16 */ {"addr.global", K::I32}, // address of script global memory + n
    /* 17 */ {"addr.local", K::I32},  // address of a local (n bytes below the locals top)
    /* 18 */ {"addr.member", K::I32}, // address of an object member (+n)
    /* 19 */ {"load.i8"},
    /* 1a */ {"load.i16"},
    /* 1b */ {"load.i32"},
    /* 1c */ {"load.f32"},
    /* 1d */ {"load.ptr"},
    /* 1e */ {"load.n", K::I32},      // pop address, push n bytes from it
    /* 1f */ {"store.i8"},            // pop address, pop value, store
    /* 20 */ {"store.i16"},
    /* 21 */ {"store.i32"},
    /* 22 */ {"store.f32"},
    /* 23 */ {"store.ptr"},
    /* 24 */ {"store.n", K::I32},     // pop address, pop n bytes, store
    /* 25 */ {"jz", K::Code},
    /* 26 */ {"jnz", K::Code},
    /* 27 */ {"jmp", K::Code},
    /* 28 */ {"call", K::Code},       // script subroutine; return address kept in the frame
    /* 29 */ {"calln", K::NativeCall},// call a native engine function by name
    /* 2a */ {"req", K::Name},        // pop object, pop priority: run object.<method> asynchronously
    /* 2b */ {"reqsw", K::Name},      // ? request and wait for it to start
    /* 2c */ {"reqew", K::Name},      // ? request and wait for it to end
    /* 2d */ {"ret.load"},            // restore the frame return address from the locals stack
    /* 2e */ {"ret.save"},            // save the frame return address to the locals stack
    /* 2f */ {"ret"},
    /* 30 */ {"end"},                 // finish the request running at the current level
    /* 31 */ {"end.level"},           // pop level, finish the request running at it
    /* 32 */ {"add"},
    /* 33 */ {"div"},
    /* 34 */ {"neg"},
    /* 35 */ {"mod"},
    /* 36 */ {"mul"},
    /* 37 */ {"sub"},
    /* 38 */ {"fadd"},
    /* 39 */ {"fdiv"},
    /* 3a */ {"fneg"},
    /* 3b */ {"fmod"},
    /* 3c */ {"fmul"},
    /* 3d */ {"fsub"},
    /* 3e */ {"and"},
    /* 3f */ {"xor"},
    /* 40 */ {"land"},
    /* 41 */ {"lor"},
    /* 42 */ {"lnot"},
    /* 43 */ {"not"},
    /* 44 */ {"or"},
    /* 45 */ {"shl"},
    /* 46 */ {"shr"},                 // arithmetic
    /* 47 */ {"itof"},
    /* 48 */ {"ftoi"},
    /* 49 */ {"eq"},
    /* 4a */ {"ne"},
    /* 4b */ {"ge"},
    /* 4c */ {"gt"},
    /* 4d */ {"le"},
    /* 4e */ {"lt"},
    /* 4f */ {"feq"},
    /* 50 */ {"fne"},
    /* 51 */ {"fge"},
    /* 52 */ {"fgt"},
    /* 53 */ {"fle"},
    /* 54 */ {"flt"},
    /* 55 */ {"cmpn.eq", K::I32},     // compare two n-byte blocks on the stack
    /* 56 */ {"cmpn.ne", K::I32},
    /* 57 */ {"cmpn.ge", K::I32},
    /* 58 */ {"cmpn.gt", K::I32},
    /* 59 */ {"cmpn.le", K::I32},
    /* 5a */ {"cmpn.lt", K::I32},
    /* 5b */ {"printf", K::I32},      // pop format string + n arguments (debug output)
    /* 5c */ {"req.byname", K::Name}, // like req, but the object is a name string
    /* 5d */ {"reqsw.byname", K::Name},
    /* 5e */ {"reqew.byname", K::Name},
    /* 5f */ {"waitobj"},             // ? pop object, pop level: wait while it runs above that level
    /* 60 */ {"flag436.clr"},         // ? thread flag 436
    /* 61 */ {"flag436.set"},         // ? thread flag 436
}};
// clang-format on

std::uint32_t operand_bytes(OperandKind kind) {
    switch (kind) {
        case K::None: return 0;
        case K::I8: return 1;
        case K::I16: return 2;
        case K::I32:
        case K::F32:
        case K::Name:
        case K::Code: return 4;
        case K::NativeCall: return 8;  // fixed part; types follow
    }
    return 0;
}

std::uint8_t byte_at(std::span<const std::byte> code, std::size_t at) {
    if (at >= code.size()) throw FormatError("bytecode truncated at offset " + nx::format("{}", at));
    return std::to_integer<std::uint8_t>(code[at]);
}

std::uint32_t be32(std::span<const std::byte> code, std::size_t at) {
    return std::uint32_t{byte_at(code, at)} << 24 | std::uint32_t{byte_at(code, at + 1)} << 16 |
           std::uint32_t{byte_at(code, at + 2)} << 8 | byte_at(code, at + 3);
}

std::uint16_t be16(std::span<const std::byte> code, std::size_t at) {
    return static_cast<std::uint16_t>(byte_at(code, at) << 8 | byte_at(code, at + 1));
}

void put_be16(nx::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v >> 8));
    out.push_back(static_cast<std::byte>(v));
}

void put_be32(nx::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>(v >> shift));
}

}  // namespace

const OpcodeInfo& opcode_info(std::uint8_t opcode) {
    if (opcode >= kOpcodeCount) throw FormatError("invalid script opcode " + nx::format("{}", opcode));
    return kOpcodes[opcode];
}

std::optional<std::uint8_t> opcode_from_mnemonic(nx::string_view mnemonic) {
    for (std::size_t i = 0; i < kOpcodes.size(); ++i)
        if (kOpcodes[i].mnemonic == mnemonic) return static_cast<std::uint8_t>(i);
    return std::nullopt;
}

std::uint32_t instruction_length(std::span<const std::byte> code, std::size_t offset) {
    const auto op = byte_at(code, offset);
    const auto& info = opcode_info(op);
    std::uint32_t length = 1 + operand_bytes(info.operand);
    if (info.operand == K::NativeCall) length += 2u * byte_at(code, offset + 6);
    if (offset + length > code.size()) throw FormatError("bytecode truncated at offset " + nx::format("{}", offset));
    return length;
}

Instruction decode(std::span<const std::byte> code, std::size_t offset) {
    Instruction insn;
    insn.offset = static_cast<std::uint32_t>(offset);
    insn.length = instruction_length(code, offset);
    insn.opcode = byte_at(code, offset);
    const std::size_t at = offset + 1;
    switch (opcode_info(insn.opcode).operand) {
        case K::None: break;
        case K::I8: insn.imm = static_cast<std::int8_t>(byte_at(code, at)); break;
        case K::I16: insn.imm = static_cast<std::int16_t>(be16(code, at)); break;
        case K::I32:
        case K::Name:
        case K::Code: insn.imm = static_cast<std::int32_t>(be32(code, at)); break;
        case K::F32: insn.fimm = std::bit_cast<float>(be32(code, at)); break;
        case K::NativeCall: {
            insn.call.name = be32(code, at);
            insn.call.has_result = byte_at(code, at + 4);
            const auto count = byte_at(code, at + 5);
            insn.call.pop_slots = static_cast<std::int16_t>(be16(code, at + 6));
            insn.call.types.resize(count);
            for (std::size_t i = 0; i < count; ++i) insn.call.types[i] = be16(code, at + 8 + 2 * i);
            insn.imm = static_cast<std::int32_t>(insn.call.name);
            break;
        }
    }
    return insn;
}

nx::vector<Instruction> decode_all(std::span<const std::byte> code) {
    nx::vector<Instruction> out;
    for (std::size_t pc = 0; pc < code.size();) {
        out.push_back(decode(code, pc));
        pc += out.back().length;
    }
    return out;
}

void encode(const Instruction& insn, nx::vector<std::byte>& out) {
    out.push_back(static_cast<std::byte>(insn.opcode));
    switch (opcode_info(insn.opcode).operand) {
        case K::None: break;
        case K::I8: out.push_back(static_cast<std::byte>(insn.imm)); break;
        case K::I16: put_be16(out, static_cast<std::uint16_t>(insn.imm)); break;
        case K::I32:
        case K::Name:
        case K::Code: put_be32(out, static_cast<std::uint32_t>(insn.imm)); break;
        case K::F32: put_be32(out, std::bit_cast<std::uint32_t>(insn.fimm)); break;
        case K::NativeCall:
            if (insn.call.types.size() > 0xFF) throw FormatError("native call has too many argument types");
            put_be32(out, insn.call.name);
            out.push_back(static_cast<std::byte>(insn.call.has_result));
            out.push_back(static_cast<std::byte>(insn.call.types.size()));
            put_be16(out, static_cast<std::uint16_t>(insn.call.pop_slots));
            for (auto t : insn.call.types) put_be16(out, t);
            break;
    }
}

}  // namespace ff7cc::atel
