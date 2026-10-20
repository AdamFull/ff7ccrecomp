#include "ff7cc/formats/strings.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include "psprecore/error.hpp"
#include "core/foundation/strings/format.h"

namespace ff7cc::strings {

namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::uint16_t kGameGlyphs = 96;
// Fields: room kept free at the end of the zeros after a string (they may hold a field of the record that is 0).
constexpr std::size_t kFieldMargin = 2;
// The game's glyphs 0-95 in ASCII, where they have one (0: none): inline names' copies.
constexpr char kAscii[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
                          "!? \"',.:;-/()&<>%[]\0+#_|^\0~*=\0\0\0\0\0";
static_assert(sizeof kAscii == 97);

std::uint32_t read32(std::span<const u8> d, std::size_t at) {
    if (at + 4 > d.size()) throw Exception("strings: truncated");
    std::uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

std::uint16_t read16(std::span<const u8> d, std::size_t at) {
    if (at + 2 > d.size()) throw Exception("strings: truncated");
    std::uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

void write16(std::span<u8> d, std::size_t at, std::uint16_t v) { std::memcpy(d.data() + at, &v, 2); }

constexpr std::size_t string_size(std::size_t glyphs) { return (4 + 2 * glyphs + 3) & ~std::size_t{3}; }

// A string's header at `at`: its glyph count, or none.
std::optional<std::size_t> string_at(std::span<const u8> d, std::size_t at) {
    if (at + 4 > d.size() || read16(d, at + 2) != kStringFlag) return std::nullopt;
    const std::size_t n = read16(d, at);
    if (at + 4 + 2 * n > d.size()) return std::nullopt;
    return n;
}

Glyphs glyphs_at(std::span<const u8> d, std::size_t at, std::size_t n) {
    Glyphs g(n);
    for (std::size_t i = 0; i < n; ++i) g[i] = read16(d, at + 4 + 2 * i);
    return g;
}

}  // namespace

bool is_table(std::span<const u8> file) {
    if (file.size() < kHeaderSize || read32(file, 0) != 1 || read32(file, 28) != kHeaderSize) return false;
    const std::size_t record_size = read32(file, 20), bytes = read32(file, 24);
    return record_size >= 8 && bytes % record_size == 0 && kHeaderSize + bytes <= file.size() &&
           string_at(file, kHeaderSize + bytes).has_value();
}

Table parse_table(std::span<const u8> file) {
    if (!is_table(file)) throw Exception("strings: not a string table");
    Table t;
    t.header_size = kHeaderSize;
    t.record_size = read32(file, 20);
    t.count = read32(file, 24) / t.record_size;
    const std::size_t pool = t.header_size + t.count * t.record_size;
    // The pool's strings in order: (offset in the pool, size).
    nx::vector<std::pair<std::size_t, std::size_t>> pooled;
    for (std::size_t at = pool; at < file.size();) {
        const auto n = string_at(file, at);
        if (!n) throw Exception(nx::format("strings: no string at 0x{:X}", at));
        pooled.emplace_back(at - pool, string_size(*n));
        at += string_size(*n);
    }
    if (pooled.size() != 2 * t.count)
        throw Exception(nx::format("strings: {} strings for {} records", pooled.size(), t.count));
    // Where every record names its two strings.
    const auto names_at = [&](std::size_t k) {
        for (std::size_t r = 0; r < t.count; ++r) {
            const std::size_t at = t.header_size + r * t.record_size + k;
            for (std::size_t s = 0; s < 2; ++s)
                if (read16(file, at + 4 * s) != pooled[2 * r + s].first ||
                    read16(file, at + 4 * s + 2) != pooled[2 * r + s].second)
                    return false;
        }
        return true;
    };
    std::size_t k = 0;
    while (k + 8 <= t.record_size && !names_at(k)) k += 2;
    if (k + 8 > t.record_size) throw Exception("strings: records do not name the pool's strings");
    t.reference = k;
    t.records.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(pool));
    t.strings.resize(t.count);
    for (std::size_t i = 0; i < pooled.size(); ++i) {
        const std::size_t at = pool + pooled[i].first;
        t.strings[i / 2][i % 2] = glyphs_at(file, at, read16(file, at));
    }
    return t;
}

nx::vector<u8> serialize(const Table& t) {
    nx::vector<u8> out = t.records;
    const std::size_t pool = out.size();
    for (std::size_t r = 0; r < t.count; ++r) {
        for (std::size_t s = 0; s < 2; ++s) {
            const Glyphs& g = t.strings[r][s];
            const std::size_t offset = out.size() - pool, size = string_size(g.size());
            if (offset + size > 0xFFFF || g.size() > 0xFFFF) throw Exception("strings: the pool outgrows 64 KB");
            out.resize(out.size() + size, 0);
            write16(out, pool + offset, static_cast<std::uint16_t>(g.size()));
            write16(out, pool + offset + 2, kStringFlag);
            for (std::size_t i = 0; i < g.size(); ++i) write16(out, pool + offset + 4 + 2 * i, g[i]);
            const std::size_t ref = t.header_size + r * t.record_size + t.reference + 4 * s;
            write16(out, ref, static_cast<std::uint16_t>(offset));
            write16(out, ref + 2, static_cast<std::uint16_t>(size));
        }
    }
    return out;
}

nx::vector<Field> fields(std::span<const u8> file) {
    nx::vector<Field> out;
    for (std::size_t at = 0; at + 4 <= file.size(); at += 2) {
        const auto n = string_at(file, at);
        if (!n || *n == 0) continue;
        Glyphs g = glyphs_at(file, at, *n);
        if (std::ranges::any_of(g, [](std::uint16_t x) { return x >= kGameGlyphs; })) continue;
        std::size_t end = at + 4 + 2 * *n;
        while (end + 2 <= file.size() && read16(file, end) == 0) end += 2;
        const std::size_t room = (end - at - 4) / 2;
        out.push_back({at, std::move(g), room > *n + kFieldMargin ? room - kFieldMargin : *n});
        at += 4 + 2 * *n - 2;
    }
    return out;
}

void write_field(std::span<u8> file, const Field& field, std::span<const std::uint16_t> glyphs) {
    if (glyphs.size() > field.capacity)
        throw Exception(nx::format("{} characters, at most {}", glyphs.size(), field.capacity));
    const std::size_t old_end = field.offset + 4 + 2 * field.glyphs.size();
    const std::size_t end = field.offset + 4 + 2 * glyphs.size();
    std::memset(file.data() + field.offset, 0, std::max(old_end, end) - field.offset);
    write16(file, field.offset, static_cast<std::uint16_t>(glyphs.size()));
    write16(file, field.offset + 2, kStringFlag);
    for (std::size_t i = 0; i < glyphs.size(); ++i) write16(file, field.offset + 4 + 2 * i, glyphs[i]);
}

nx::vector<InlineName> inline_names(std::span<const u8> module) {
    nx::vector<InlineName> out;
    for (std::size_t at = 0; at + 4 <= module.size(); at += 2) {
        const auto n = string_at(module, at);
        if (!n || *n == 0) continue;
        const std::size_t ascii = at + string_size(*n);
        if (ascii + *n >= module.size()) continue;
        Glyphs g = glyphs_at(module, at, *n);
        bool copy = true;
        for (std::size_t i = 0; i < *n && copy; ++i)
            copy = g[i] < kGameGlyphs && kAscii[g[i]] && module[ascii + i] == static_cast<u8>(kAscii[g[i]]);
        if (!copy || module[ascii + *n] != 0) continue;
        const std::size_t bytes = string_size(*n) + *n + 1;
        // the most glyphs whose 4-aligned string leaves a byte for the zero
        const std::size_t capacity = ((bytes - 1) & ~std::size_t{3}) / 2 - 2;
        out.push_back({at, std::move(g), bytes, capacity});
        at = ((at + bytes + 1) & ~std::size_t{1}) - 2;  // (strings start at even offsets; the copy has any length)
    }
    return out;
}

void write_inline(std::span<u8> module, const InlineName& name, std::span<const std::uint16_t> glyphs) {
    if (glyphs.size() > name.capacity)
        throw Exception(nx::format("{} characters, at most {}", glyphs.size(), name.capacity));
    std::memset(module.data() + name.offset, 0, name.bytes);
    write16(module, name.offset, static_cast<std::uint16_t>(glyphs.size()));
    write16(module, name.offset + 2, kStringFlag);
    for (std::size_t i = 0; i < glyphs.size(); ++i) write16(module, name.offset + 4 + 2 * i, glyphs[i]);
    // The ASCII copy: as much of the old one as fits, padded with spaces, then the zero at the old end.
    const std::size_t ascii = name.offset + string_size(glyphs.size()), end = name.offset + name.bytes - 1;
    for (std::size_t i = 0; ascii + i < end; ++i)
        module[ascii + i] = static_cast<u8>(i < name.glyphs.size() ? kAscii[name.glyphs[i]] : ' ');
}

}  // namespace ff7cc::strings
