#pragma once

// Glyph strings: the form of menu text (and of the name table, archive entry 1). A string is
//   u32 glyph count | 0x2000 << 16, then u16 glyph indices (the font's, docs/text_rendering.md)
// and they come in three kinds of places:
//
// String tables (archive entries 10616-10621: accessories, items, materials, materia, commands, abilities), loaded
// whole by the game, which allocates for their size:
//   0x00 u32 1, u32 0, u32 0, u32 0, u32 last record index, u32 record size, u32 bytes of records, u32 header size
//   then the records, then the string pool. Each record names its two strings (name, description) by
//   { u16 offset in the pool, u16 size (header + glyphs, rounded up to 4) } at the same place in every record; the
//   strings follow each other in the pool, 4-aligned, in record order. A table can be rebuilt with strings of any
//   length (the pool stays under 64 KB).
//
// Field files (archive entries 1435: mission categories and titles; 1436: mail folders, senders, subjects): each
// string sits in a fixed field padded with zeros, between other data of its record. A string can grow into the
// zeros after it, less a margin (fields end with data the zeros may be part of).
//
// Inline names (battle script modules: enemies and their actions): a glyph string, 4-aligned, followed by the same
// text in ASCII with a terminating zero, then the record's next string: the game walks the record by these lengths.
// It never reads the ASCII copy (the Behemoth fight draws the same pictures with it overwritten), so a name can grow
// into its bytes, the ASCII shortened to keep the total: everything after the name stays where it was.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "ff7cc/core.hpp"

namespace ff7cc::strings {

inline constexpr std::uint16_t kStringFlag = 0x2000;
inline constexpr std::size_t kTableEntries[] = {10616, 10617, 10618, 10619, 10620, 10621};
inline constexpr std::size_t kFieldEntries[] = {1435, 1436};

using Glyphs = nx::vector<std::uint16_t>;

struct Table {
    nx::vector<u8> records;  // the header and the records, as in the file (string references rewritten on serialize)
    std::size_t header_size = 0, record_size = 0, count = 0;
    std::size_t reference = 0;  // where a record names its strings: name at +0, description at +4
    nx::vector<std::array<Glyphs, 2>> strings;  // per record: name, description
};

bool is_table(std::span<const u8> file);
Table parse_table(std::span<const u8> file);  // throws Exception
nx::vector<u8> serialize(const Table& table);  // throws Exception if the pool outgrows 64 KB

struct Field {
    std::size_t offset = 0;  // of the string's header in the file
    Glyphs glyphs;
    std::size_t capacity = 0;  // glyphs the field holds
};

// The glyph strings of a field file whose glyphs are all the game's (0-95), with the room each has.
nx::vector<Field> fields(std::span<const u8> file);
// Writes `glyphs` into `field` (zeros after them). Throws Exception past its capacity.
void write_field(std::span<u8> file, const Field& field, std::span<const std::uint16_t> glyphs);

struct InlineName {
    std::size_t offset = 0;  // of the glyph string's header
    Glyphs glyphs;
    std::size_t bytes = 0;     // the glyph string, its ASCII copy and the zero
    std::size_t capacity = 0;  // glyphs that fit in `bytes` (with an empty ASCII copy)
};

// The inline names of a module's bytes (glyphs all the game's, followed by their ASCII copy), in file order.
nx::vector<InlineName> inline_names(std::span<const u8> module);
// Writes `glyphs` over `name`, the ASCII copy cut or padded to keep its bytes. Throws Exception past its capacity.
void write_inline(std::span<u8> module, const InlineName& name, std::span<const std::uint16_t> glyphs);

}  // namespace ff7cc::strings
