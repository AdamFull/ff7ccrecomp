#include "ff7cc/formats/font.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psprecore/error.hpp"
#include "core/foundation/strings/format.h"

namespace ff7cc::font {

namespace {

std::uint32_t read32(std::span<const u8> d, std::size_t at) {
    if (at + 4 > d.size()) throw Exception("font: truncated");
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

std::uint16_t read16(std::span<const u8> d, std::size_t at) {
    if (at + 2 > d.size()) throw Exception("font: truncated");
    std::uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

// Byte offset of texel byte (x, y) of a 4-bit swizzled texture: 16-byte x 8-row blocks, row by row.
std::size_t swizzled_offset(int byte_x, int y, int row_bytes) {
    const int blocks_per_row = row_bytes / 16;
    const int block = (y / 8) * blocks_per_row + byte_x / 16;
    return static_cast<std::size_t>(block * 128 + (y % 8) * 16 + byte_x % 16);
}

}  // namespace

nx::vector<Item> items(std::span<const u8> file) {
    const std::uint32_t count = read32(file, 0);
    if (count == 0 || count > 256) throw Exception("font: not a font file");
    nx::vector<Item> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        Item item{read32(file, 4 + 8 * i), read32(file, 8 + 8 * i)};
        if (item.offset + item.size > file.size()) throw Exception("font: item past the end of the file");
        out.push_back(item);
    }
    return out;
}

Picture picture(std::span<const u8> file, const Item& item) {
    if (item.size < 16 || std::memcmp(file.data() + item.offset, "IMG", 4) != 0) throw Exception("font: not a picture");
    Picture p;
    p.width = read16(file, item.offset + 4);
    p.height = read16(file, item.offset + 6);
    p.buffer_width = read16(file, item.offset + 8);
    p.colours = read16(file, item.offset + 10);
    p.bytes = read32(file, item.offset + 12);
    p.texels = item.offset + 16;
    if (p.colours != 16 || p.texels + p.bytes > file.size() ||
        p.bytes != static_cast<std::size_t>(p.buffer_width) * static_cast<std::size_t>(p.height) / 2)
        throw Exception("font: unsupported picture (only 4-bit swizzled sheets)");
    return p;
}

nx::vector<std::uint32_t> palette(std::span<const u8> file, const Item& item) {
    if (item.size != 64) throw Exception("font: not a palette");
    nx::vector<std::uint32_t> colours(16);
    std::memcpy(colours.data(), file.data() + item.offset, 64);
    return colours;
}

nx::vector<std::uint8_t> unswizzle4(std::span<const u8> texels, int width, int height) {
    const int row_bytes = width / 2;
    nx::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int bx = 0; bx < row_bytes; ++bx) {
            const auto b = static_cast<std::uint8_t>(texels[swizzled_offset(bx, y, row_bytes)]);
            out[static_cast<std::size_t>(y * width + bx * 2)] = b & 15;
            out[static_cast<std::size_t>(y * width + bx * 2 + 1)] = b >> 4;
        }
    return out;
}

void swizzle4(std::span<const std::uint8_t> indices, int width, int height, std::span<u8> texels) {
    const int row_bytes = width / 2;
    for (int y = 0; y < height; ++y)
        for (int bx = 0; bx < row_bytes; ++bx) {
            const std::uint8_t lo = indices[static_cast<std::size_t>(y * width + bx * 2)] & 15;
            const std::uint8_t hi = indices[static_cast<std::size_t>(y * width + bx * 2 + 1)] & 15;
            texels[swizzled_offset(bx, y, row_bytes)] = static_cast<u8>(lo | (hi << 4));
        }
}

nx::vector<ExtraGlyph> parse_glyphs(nx::string_view text) {
    nx::vector<ExtraGlyph> out;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // a UTF-8 BOM
    int line_number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        nx::string_view line = text.substr(0, end);
        text.remove_prefix(end == nx::string_view::npos ? text.size() : end + 1);
        ++line_number;
        if (const std::size_t hash = line.find('#'); hash != nx::string_view::npos) line = line.substr(0, hash);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;
        const auto fail = [&](const char* why) {
            throw Exception(nx::format("glyphs.txt line {}: {}", line_number, why));
        };
        // <index> <character> <advance> [<small font advance>], or <index> <character> - (an alias of one of the
        // game's glyphs)
        nx::vector<nx::string_view> fields;
        for (std::size_t at = 0; at < line.size();) {
            const std::size_t first = line.find_first_not_of(" \t", at);
            if (first == nx::string_view::npos) break;
            const std::size_t last = std::min(line.find_first_of(" \t", first), line.size());
            fields.push_back(line.substr(first, last - first));
            at = last;
        }
        if (fields.size() < 3 || fields.size() > 4)
            fail("expected <glyph index> <character> <advance> [<small advance>]");
        ExtraGlyph g;
        g.index = std::atoi(nx::string(fields[0]).c_str());
        const bool alias = fields[2] == "-";
        g.advance = alias ? 0 : std::atoi(nx::string(fields[2]).c_str());
        if (fields.size() == 4) g.small_advance = std::atoi(nx::string(fields[3]).c_str());
        g.text = nx::string(fields[1]);
        if (alias) {
            if (g.index < 0 || g.index >= kFirstGlyphs) fail("an alias names one of the game's glyphs (0-95)");
            if (g.text.empty()) fail("bad character");
            out.push_back(std::move(g));
            continue;
        }
        if (!free_glyph(g.index)) fail("the glyph index is not a free cell (96-113, 228-341)");
        if (g.text.empty() || g.advance <= 0 || g.advance > 2 * kCellWidth) fail("bad character or advance");
        if (g.small_advance < 0 || g.small_advance > 2 * kSmallCellWidth) fail("bad small font advance");
        if (g.small_advance && !free_small_glyph(g.index))
            fail("the glyph index is not a free cell of the small sheet (96-113, 229-275)");
        out.push_back(std::move(g));
    }
    return out;
}

}  // namespace ff7cc::font
