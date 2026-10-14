#pragma once

// "MBD" message tables: the game's dialogue and menu text (664 resources inside Atel modules).
//
// Layout (little-endian):
//   "MBD\0"
//   messages until the end of the data:
//     u32 page_count
//     { u32 length, u32 offset }[page_count]   length in 4-byte tokens; offset from the message start
//     page data: tokens { u16 code, u16 arg }
//
// Tokens: code 0x0080 draws glyph `arg` of the font. Codes with low byte 0x40 are controls numbered by the high
// byte: 0x02 newline, 0x03 end of page (wait for a button, then clear), 0x04 end of message, 0x05 choice list
// (arg high byte = number of options), others insert values or button icons. Every page ends with 0x03, the last
// one with 0x04. Parsing and serializing reproduces the data byte for byte.
//
// Text form (UTF-8) for editing: glyphs map through a Charset; "\n" is a newline, "{PAGE}" separates pages,
// "{CHOICE:200}" is a choice list, "{C10}" / "{C10:0100}" other controls (number and argument in hex), and
// "{G:150}" a glyph the charset does not name. The end-of-page and end-of-message tokens are implied.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"
#include "ff7cc/core.hpp"

namespace ff7cc::mbd {

inline constexpr std::uint16_t kGlyph = 0x0080;
inline constexpr std::uint8_t kNewline = 0x02, kPageEnd = 0x03, kMessageEnd = 0x04, kChoice = 0x05;

struct Token {
    std::uint16_t code = kGlyph;
    std::uint16_t arg = 0;

    bool is_glyph() const { return code == kGlyph; }
    bool is_control() const { return (code & 0xFF) == 0x40; }
    std::uint8_t control() const { return static_cast<std::uint8_t>(code >> 8); }
    static Token make_control(std::uint8_t number, std::uint16_t arg = 0) {
        return {static_cast<std::uint16_t>((number << 8) | 0x40), arg};
    }
    friend bool operator==(const Token&, const Token&) = default;
};

struct Message {
    nx::vector<nx::vector<Token>> pages;  // each page includes its end token (0x03 or 0x04)
    friend bool operator==(const Message&, const Message&) = default;
};

bool is_mbd(std::span<const std::byte> data);
nx::vector<Message> parse(std::span<const std::byte> data);  // throws FormatError
nx::vector<std::byte> serialize(const nx::vector<Message>& messages);

// Scripts address messages by their byte offset in the table (e.g. Mes(2128, 18, ...) = offset 2128 of resource
// 18), so translated tables must keep every message where it was. A changed message that fits in its original
// space is written there (the rest is zero-filled); a longer one keeps its header at the original offset while its
// pages move to the end of the table (page offsets are relative to the message start). `changes` maps message
// indices of `original` to their new content.
nx::vector<std::byte> patch(std::span<const std::byte> original, const nx::map<std::size_t, Message>& changes);

// Glyph index -> UTF-8 text. Unnamed glyphs are written as {G:n}.
struct Charset {
    std::array<nx::string, 256> glyphs;

    // The font of the European English release: glyphs 0-95 of its sheet (the text uses no others).
    static Charset european();
};

nx::string to_text(const Message& message, const Charset& charset);
// Throws FormatError on characters the charset cannot encode or malformed tags.
Message from_text(nx::string_view text, const Charset& charset);

}  // namespace ff7cc::mbd
