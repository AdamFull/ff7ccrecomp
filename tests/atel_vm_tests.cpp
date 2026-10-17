#include <cstring>

#include "psprecore/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/atel_disasm.hpp"
#include "ff7cc/formats/atel_vm.hpp"
#include "test_main.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;
using namespace std::string_view_literals;

namespace {

nx::vector<u8> bytes(nx::string_view text) {
    nx::vector<u8> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

}  // namespace

TEST_CASE("atel vm: lengths follow the game's table") {
    // Spot checks against BOOT.BIN 0x46C10: 1-byte ops, 0x07 = 2, 0x08 = 3, 5-byte ops, variable 0x29.
    CHECK(atel::instruction_length(bytes("\x14"sv), 0) == 1);
    CHECK(atel::instruction_length(bytes("\x07\xff"sv), 0) == 2);
    CHECK(atel::instruction_length(bytes("\x08\x00\x01"sv), 0) == 3);
    CHECK(atel::instruction_length(bytes("\x27\x00\x00\x00\x10"sv), 0) == 5);
    CHECK(atel::instruction_length(bytes("\x5b\x00\x00\x00\x02"sv), 0) == 5);
    CHECK(atel::instruction_length(bytes("\x29\x00\x00\x00\x04\x01\x02\x00\x03\x00\x01\x00\x02"sv), 0) == 13);
}

TEST_CASE("atel vm: decode immediates (big-endian, signed)") {
    CHECK(atel::decode(bytes("\x07\xfe"sv), 0).imm == -2);
    CHECK(atel::decode(bytes("\x08\xff\x00"sv), 0).imm == -256);
    CHECK(atel::decode(bytes("\x09\x12\x34\x56\x78"sv), 0).imm == 0x12345678);
    CHECK(atel::decode(bytes("\x0a\x3f\xc0\x00\x00"sv), 0).fimm == 1.5f);

    const auto call = atel::decode(bytes("\x29\x00\x00\x00\x04\x01\x02\x00\x03\x00\x01\x00\x02"sv), 0);
    CHECK(call.call.name == 4);
    CHECK(call.call.has_result == 1);
    CHECK(call.call.pop_slots == 3);
    CHECK(call.call.types == nx::vector<std::uint16_t>({1, 2}));
}

TEST_CASE("atel vm: decode/encode round-trip") {
    const auto code = bytes(
        "\x05\x07\x80\x08\x12\x34\x09\xff\xff\xff\xff\x0a\x40\x49\x0f\xdb\x0b\x00\x00\x00\x05"
        "\x29\x00\x00\x00\x09\x00\x01\xff\xfe\x00\x07\x25\x00\x00\x00\x00\x01"sv);
    nx::vector<u8> out;
    for (const auto& insn : atel::decode_all(code)) atel::encode(insn, out);
    CHECK(out == code);
}

TEST_CASE("atel vm: invalid or truncated bytecode is rejected") {
    CHECK_THROWS_AS(atel::decode(bytes("\x62"sv), 0), Exception);
    CHECK_THROWS_AS(atel::decode(bytes("\x09\x00\x00"sv), 0), Exception);
    CHECK_THROWS_AS(atel::decode_all(bytes("\x29\x00\x00\x00\x00\x00\x02\x00\x00\x00"sv)), Exception);
}

TEST_CASE("atel vm: mnemonics are unique and reversible") {
    nx::set<nx::string_view> seen;
    for (std::size_t op = 0; op < atel::kOpcodeCount; ++op) {
        const auto name = atel::opcode_info(static_cast<std::uint8_t>(op)).mnemonic;
        CHECK(!name.empty());
        CHECK(seen.insert(name).second);
        CHECK(atel::opcode_from_mnemonic(name) == op);
    }
}

TEST_CASE("atel disasm labels methods, calls and jumps") {
    atel::Module m;
    m.names = bytes("Obj\0main\0MapJump\0hi\"\x83\x4c\0"sv);
    // 0000 call sub_000B; 0005 halt; 0006 jmp L_0006 ... keep it simple:
    m.code = bytes(
        "\x28\x00\x00\x00\x0b"          // 0000 call 000B
        "\x01"                          // 0005 halt
        "\x27\x00\x00\x00\x05"          // 0006 jmp 0005
        "\x0b\x00\x00\x00\x11"          // 000B push.str "hi\"\x83\x4c"
        "\x29\x00\x00\x00\x09\x00\x00\x00\x01"  // 0010 calln "MapJump", pop=1
        "\x2f"sv);                      // 0019 ret
    m.objects = {atel::Object{1, 0, 0, {{4, 0}}}};
    // Collapse column padding so the checks don't depend on alignment.
    nx::string text;
    for (char c : atel::disassemble(m))
        if (c != ' ' || text.empty() || text.back() != ' ') text += c;
    CHECK(text.find("Obj.main:\n 0000 call sub_000B\n") != nx::string::npos);
    CHECK(text.find("L_0005:\n 0005 halt\n") != nx::string::npos);
    CHECK(text.find(" 0006 jmp L_0005\n") != nx::string::npos);
    CHECK(text.find(R"(sub_000B:)" "\n" R"( 000B push.str "hi\"\x83L")") != nx::string::npos);
    CHECK(text.find(R"( 0010 calln "MapJump", pop=1)" "\n") != nx::string::npos);
}
