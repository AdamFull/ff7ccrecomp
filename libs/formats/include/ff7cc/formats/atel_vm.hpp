#pragma once

// Atel script VM instruction set (reversed from BOOT.BIN: length table 0x46C10, handler table 0x1A7C50,
// interpreter loop 0x475F8).
//
// The VM is a stack machine with one value stack of 32-bit slots (ints, floats, pointers) and a byte-addressed
// locals stack. Each thread runs at one of 8 priority levels ("frames"). Immediates are big-endian.
// Binary operators pop b (top) then a and push `a op b`.
//
// Mnemonics marked "?" in the table comments are tentative: the handler is understood mechanically but its
// gameplay meaning is not yet confirmed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ff7cc::atel {

enum class OperandKind : std::uint8_t {
    None,
    I8,          // signed 8-bit immediate
    I16,         // signed 16-bit immediate
    I32,         // 32-bit immediate (integer, size, offset or count, depending on the opcode)
    F32,         // 32-bit float immediate
    Name,        // offset into the module's name pool
    Code,        // offset into the module's bytecode (jump / call target)
    NativeCall,  // opcode 0x29: see NativeCall
};

struct OpcodeInfo {
    std::string_view mnemonic;
    OperandKind operand = OperandKind::None;
};

inline constexpr std::size_t kOpcodeCount = 98;
inline constexpr std::uint8_t kOpCallNative = 0x29;

const OpcodeInfo& opcode_info(std::uint8_t opcode);  // throws FormatError for opcodes >= kOpcodeCount
std::optional<std::uint8_t> opcode_from_mnemonic(std::string_view mnemonic);

// Operands of opcode 0x29: 29 <name u32> <has_result u8> <type_count u8> <pop_slots s16> <types u16[type_count]>.
// The loader replaces `name` (a name-pool offset such as "MapJump") with the native function address.
struct NativeCall {
    std::uint32_t name = 0;
    std::uint8_t has_result = 0;
    std::int16_t pop_slots = 0;           // value-stack slots consumed by the call
    std::vector<std::uint16_t> types;     // per-argument descriptors (meaning not yet known)
};

struct Instruction {
    std::uint32_t offset = 0;  // position in the bytecode
    std::uint32_t length = 0;
    std::uint8_t opcode = 0;
    std::int32_t imm = 0;      // I8/I16/I32/Name/Code operands (sign-extended for I8/I16)
    float fimm = 0.0f;         // F32 operand
    NativeCall call;           // opcode 0x29 only
};

// Length in bytes of the instruction at `offset`; throws FormatError if truncated or invalid.
std::uint32_t instruction_length(std::span<const std::byte> code, std::size_t offset);

Instruction decode(std::span<const std::byte> code, std::size_t offset);

// Decodes the whole bytecode linearly. Throws FormatError on an invalid opcode or truncation.
std::vector<Instruction> decode_all(std::span<const std::byte> code);

// Appends the encoding of `insn` (its opcode/operands; offset and length are ignored).
void encode(const Instruction& insn, std::vector<std::byte>& out);

}  // namespace ff7cc::atel
