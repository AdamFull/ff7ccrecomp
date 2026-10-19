#pragma once

// The game's fonts (docs/text_rendering.md): archive entry 1472 holds both glyph sheets and their palettes, and mods
// can add characters to the regular font.
//
// The file is a list of items:
//   u32 count, { u32 offset, u32 size }[count]
// Items are pictures or palettes. A picture: "IMG\0", u16 width, u16 height, u16 buffer width, u16 colours
// (16: 4-bit indices), u32 texel bytes, then the texels (swizzled: 16-byte x 8-row blocks). A palette: 16 colours,
// RGBA8. Item 0 is the regular font's sheet (13x14 cells, 19 to a row), item 3 the palette text is drawn with; item
// 6 is the small font's sheet.
//
// Extra characters (mods/<mod>/font/glyphs.txt), one per line: `<glyph index> <character> <advance>`, e.g.
// `228 é 6`; `#` starts a comment. Free cells of the regular sheet: glyphs 96-113 (with italics in the matching
// cells of row 11) and 228-341 (rows 12-17, regular only).

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "ff7cc/core.hpp"

namespace ff7cc::font {

inline constexpr std::size_t kFontEntry = 1472;  // archive entry
inline constexpr std::size_t kRegularSheet = 0, kTextPalette = 3;
inline constexpr int kCellWidth = 13, kCellHeight = 14, kColumns = 19;
inline constexpr int kFirstGlyphs = 96;  // the game's own characters
// Glyph indices whose cells are free in the regular sheet.
constexpr bool free_glyph(int index) { return (index >= 96 && index <= 113) || (index >= 228 && index <= 341); }

struct Item {
    std::size_t offset = 0, size = 0;
};

struct Picture {
    int width = 0, height = 0, buffer_width = 0, colours = 0;
    std::size_t texels = 0, bytes = 0;  // offset of the texels in the file, their size
};

nx::vector<Item> items(std::span<const u8> file);                     // throws FormatError
Picture picture(std::span<const u8> file, const Item& item);         // throws FormatError if not a picture
// A palette item as RGBA8 colours (R in the low byte).
nx::vector<std::uint32_t> palette(std::span<const u8> file, const Item& item);

// 4-bit swizzled texels <-> one palette index per texel, rows of `width`.
nx::vector<std::uint8_t> unswizzle4(std::span<const u8> texels, int width, int height);
void swizzle4(std::span<const std::uint8_t> indices, int width, int height, std::span<u8> texels);

struct ExtraGlyph {
    int index = 0;
    nx::string text;  // UTF-8
    int advance = 0;  // pixels the pen moves after the glyph; 0: an alias of one of the game's glyphs
};
// Parses glyphs.txt: "<index> <character> <advance>" for a glyph drawn into a free cell, "<index> <character> -" with
// one of the game's glyphs (0-95) for an alias: a character written with that glyph (Cyrillic А as Latin A). Throws
// FormatError naming the line on malformed input or an index outside the free cells.
nx::vector<ExtraGlyph> parse_glyphs(nx::string_view text);

}  // namespace ff7cc::font
