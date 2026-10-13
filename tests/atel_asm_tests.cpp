#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "ff7cc/formats/atel_asm.hpp"
#include "ff7cc/formats/atel_disasm.hpp"
#include "ff7cc/formats/atel_vm.hpp"
#include "test_main.hpp"

using namespace ff7cc;
using namespace std::string_view_literals;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

atel::Module module_with_method() {
    atel::Module m;
    m.names = bytes("Obj\0main\0MapJump\0"sv);
    m.objects = {atel::Object{1, 0, 0, {{4, 0}}}};
    return m;
}

}  // namespace

TEST_CASE("atel asm: assembles labels, literals and native calls") {
    auto m = module_with_method();
    atel::assemble_into(m, R"(
        ; a hand-written script
        Obj.main:
            push.i8   -1
            push.f32  0.5
            push.f32  bits(0x7fc00001)      ; NaN with payload
            push.str  "MapJump"             ; existing string: reuses offset 9
            push.str  "new\x83L"            ; new string: appended
            calln     "MapJump", pop=2, ret, types=[0x0001, 0x0000]
        loop:
            jnz       loop
            0010 halt                       ; offset column is ignored
    )");
    const auto insns = atel::decode_all(m.code);
    CHECK(insns.size() == 8);
    CHECK(insns[0].imm == -1);
    CHECK(insns[1].fimm == 0.5f);
    CHECK(std::bit_cast<std::uint32_t>(insns[2].fimm) == 0x7fc00001u);
    CHECK(insns[3].imm == 9);
    CHECK(m.name_at(static_cast<std::uint32_t>(insns[4].imm)) == "new\x83L");
    CHECK(insns[4].imm == 17);  // appended after the original 17-byte pool
    CHECK(insns[5].call.name == 9 && insns[5].call.has_result == 1 && insns[5].call.pop_slots == 2);
    CHECK(insns[5].call.types == std::vector<std::uint16_t>({1, 0}));
    CHECK(insns[6].imm == static_cast<std::int32_t>(insns[6].offset));  // jnz to itself
    CHECK(insns[7].opcode == 0x01);
    CHECK(m.objects[0].methods[0].code_offset == 0);
}

TEST_CASE("atel asm: method entry points follow their labels") {
    auto m = module_with_method();
    atel::assemble_into(m, "halt\nhalt\nObj.main:\nret\n");
    CHECK(m.objects[0].methods[0].code_offset == 2);
}

TEST_CASE("atel asm: disassembly re-assembles byte for byte") {
    auto m = module_with_method();
    m.code = bytes(
        "\x28\x00\x00\x00\x0b\x01\x27\x00\x00\x00\x05\x0a\x40\x49\x0f\xdb\x08\xff\xfe"
        "\x29\x00\x00\x00\x09\x01\x01\x00\x01\x00\x00\x2f"sv);
    auto copy = m;
    atel::assemble_into(copy, atel::disassemble(m));
    CHECK(copy.code == m.code);
    CHECK(copy.names == m.names);
}

TEST_CASE("atel asm: errors carry line numbers and leave the module untouched") {
    auto m = module_with_method();
    const auto before = m;
    const auto expect_error = [&](std::string_view listing, std::string_view needle) {
        try {
            atel::assemble_into(m, listing);
            CHECK(!"expected FormatError");
        } catch (const FormatError& e) {
            CHECK(std::string(e.what()).find(needle) != std::string::npos);
        }
        CHECK(m == before);
    };
    expect_error("Obj.main:\nfrobnicate\n", "line 2: unknown mnemonic");
    expect_error("Obj.main:\njmp nowhere\n", "undefined label 'nowhere'");
    expect_error("halt\n", "no label for method 'Obj.main'");
    expect_error("Obj.main:\npush.i8 300\n", "out of range");
    expect_error("Obj.main:\npush.str \"oops\n", "unterminated string");
}
