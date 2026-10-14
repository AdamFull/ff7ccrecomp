#include <algorithm>
#include <cstring>

#include "psprecore/error.hpp"
#include "ff7cc/formats/atel.hpp"
#include "test_main.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

using namespace ff7cc;
using namespace std::string_view_literals;

namespace {

nx::vector<std::byte> bytes(nx::string_view text) {
    nx::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

atel::Module sample_module() {
    atel::Module m;
    m.seed = 0x47FED3BA;
    m.names = bytes(nx::string_view("Root\0init\0main\0Director\0", 25));
    m.code = bytes("\x10\x00\x00\x00\x10\x2e\x06\x09\x00\x00\x00\x00\x17"sv);  // 13 bytes, arbitrary
    atel::Object director;
    director.name = 15;  // "Director"
    director.methods = {{10, 4}, {5, 0}};
    m.objects = {director};
    m.resources = {
        {bytes("MBD\0abc"sv), bytes("\x40\x02\x00\x00\x40\x04\x00\x00"sv)},  // slack carries data
        {bytes("SSCF0123456789abcdef"), {}},
        {{}, {}},
    };
    return m;
}

}  // namespace

TEST_CASE("atel name cipher matches the game LCG") {
    // "Root\0" encrypted with seed 0x47FED3BA, as stored at the start of the archive header module.
    auto names = bytes(nx::string_view("Root\0", 5));
    atel::crypt_names(names, 0x47FED3BA);
    CHECK(names == bytes("\x8c\x13\x43\x6f\x59"));
    atel::crypt_names(names, 0x47FED3BA);
    CHECK(names == bytes(nx::string_view("Root\0", 5)));
}

TEST_CASE("atel serialize/parse round-trip") {
    const auto m = sample_module();
    const auto raw = atel::serialize(m);
    CHECK(raw.size() % 16 == 0);
    CHECK(atel::is_atel(raw));

    const auto back = atel::parse(raw);
    CHECK(back.objects == m.objects);
    CHECK(back.names == m.names);
    CHECK(back.code == m.code);
    CHECK(back.resources.size() == 3);
    CHECK(back.resources[0].data == m.resources[0].data);
    // Slack is kept verbatim, then zero-filled up to the next 16-byte boundary.
    CHECK(std::equal(m.resources[0].slack.begin(), m.resources[0].slack.end(), back.resources[0].slack.begin()));
    CHECK(back.resources[1].data == m.resources[1].data);
    CHECK(back.name_at(back.objects[0].name) == "Director");
    CHECK(back.name_at(back.objects[0].methods[0].name) == "main");
    // A second pass must be byte-identical (filler and slack are now canonical).
    CHECK(atel::serialize(back) == raw);
}

TEST_CASE("atel names are stored encrypted unless flagged plain") {
    auto m = sample_module();
    const auto encrypted = atel::serialize(m);
    const auto pos = [](const nx::vector<std::byte>& v) {
        const auto r = bytes("Director");
        return std::search(v.begin(), v.end(), r.begin(), r.end()) != v.end();
    };
    CHECK(!pos(encrypted));
    m.flags |= atel::kFlagPlainNames;
    const auto plain = atel::serialize(m);
    CHECK(pos(plain));
    CHECK(atel::parse(plain).names == m.names);
}

TEST_CASE("atel resources are 16-byte aligned after resizing") {
    auto m = sample_module();
    m.resources[0].data.resize(37);  // edit a resource: following ones must move and stay aligned
    const auto back = atel::parse(atel::serialize(m));
    CHECK(back.resources[0].data.size() == 37);
    CHECK(back.resources[1].data == m.resources[1].data);
}

TEST_CASE("atel rejects malformed input") {
    CHECK_THROWS_AS(atel::parse(bytes("Atex")), FormatError);
    auto raw = atel::serialize(sample_module());
    raw.resize(30);
    CHECK_THROWS_AS(atel::parse(raw), FormatError);
    auto bad_entry = sample_module();
    bad_entry.objects[0].methods[0].code_offset = 999;
    CHECK_THROWS_AS(atel::parse(atel::serialize(bad_entry)), FormatError);
}

TEST_CASE("atel resource type tags") {
    CHECK(atel::resource_type(bytes(nx::string_view("MBD\0", 4))) == "mbd");
    CHECK(atel::resource_type(bytes("SSCF")) == "sscf");
    CHECK(atel::resource_type(bytes("RIFF")) == "at3");
    CHECK(atel::resource_type(bytes("GT\x00\x02"sv)) == "bin");
}
