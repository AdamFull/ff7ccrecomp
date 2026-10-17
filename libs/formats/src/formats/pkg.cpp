#include "ff7cc/formats/pkg.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

#include "psprecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

static_assert(std::endian::native == std::endian::little, "big-endian hosts are not supported");

namespace ff7cc::pkg {

namespace {

std::uint32_t load_u32(const u8* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void store_u32(u8* p, std::uint32_t v) { std::memcpy(p, &v, sizeof v); }

bool has_magic(std::span<const u8> head, nx::string_view magic) {
    return head.size() >= magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0;
}

struct TypeInfo {
    FileType type;
    nx::string_view name;
    nx::string_view extension;
};

constexpr std::array kTypes{
    TypeInfo{FileType::Empty, "empty", ""},
    TypeInfo{FileType::Atel, "atel", ".atel"},
    TypeInfo{FileType::Riff, "riff", ".at3"},
    TypeInfo{FileType::Psmf, "psmf", ".pmf"},
    TypeInfo{FileType::Sscf, "sscf", ".sscf"},
    TypeInfo{FileType::Unknown, "bin", ".bin"},
};

const TypeInfo& info(FileType type) {
    for (const auto& t : kTypes)
        if (t.type == type) return t;
    return kTypes.back();
}

}  // namespace

nx::vector<Entry> parse_index(std::span<const u8> fse) {
    if (fse.size() % kEntrySize != 0)
        throw Exception("discimg.fse size " + nx::format("{}", fse.size()) + " is not a multiple of 12");
    nx::vector<Entry> entries(fse.size() / kEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const u8* p = fse.data() + i * kEntrySize;
        entries[i] = Entry{load_u32(p), load_u32(p + 4), load_u32(p + 8)};
    }
    return entries;
}

nx::vector<u8> serialize_index(std::span<const Entry> entries) {
    nx::vector<u8> out(entries.size() * kEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        u8* p = out.data() + i * kEntrySize;
        store_u32(p, entries[i].sector);
        store_u32(p + 4, entries[i].size);
        store_u32(p + 8, entries[i].reserved);
    }
    return out;
}

void validate_index(std::span<const Entry> entries, std::uint64_t archive_size) {
    std::uint64_t min_offset = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (e.offset() < min_offset)
            throw Exception("entry " + nx::format("{}", i) + " overlaps the previous file");
        if (e.end() > archive_size)
            throw Exception("entry " + nx::format("{}", i) + " runs past the end of the archive");
        min_offset = e.end();
    }
}

FileType sniff(std::span<const u8> head) {
    if (head.empty()) return FileType::Empty;
    if (has_magic(head, "Atel")) return FileType::Atel;
    if (has_magic(head, "RIFF")) return FileType::Riff;
    if (has_magic(head, "PSMF")) return FileType::Psmf;
    if (has_magic(head, "SSCF")) return FileType::Sscf;
    return FileType::Unknown;
}

nx::string_view type_name(FileType type) { return info(type).name; }
nx::string_view type_extension(FileType type) { return info(type).extension; }

FileType type_from_name(nx::string_view name) {
    for (const auto& t : kTypes)
        if (t.name == name) return t.type;
    return FileType::Unknown;
}

ArchivePaths locate(nx::fs::path_view dir) {
    for (const nx::string_view sub : {"", "USRDIR", "PSP_GAME/USRDIR"}) {
        const nx::string base = path_join(dir.native(), sub);
        ArchivePaths paths{path_join(base, "discimg.fse"), path_join(base, "discimg.pkg")};
        if (is_file(paths.fse) && is_file(paths.pkg)) return paths;
    }
    throw Exception(nx::string("discimg.fse/discimg.pkg not found under '") + dir.native() + "'");
}

ArchivePaths output_paths(nx::fs::path_view dir) {
    return ArchivePaths{path_join(dir.native(), "discimg.fse"), path_join(dir.native(), "discimg.pkg")};
}

// ---------------------------------------------------------------------------------------------

Reader::Reader(const ArchivePaths& paths)
    : entries_(parse_index(value_or_throw(io::read_file(paths.fse)))), pkg_(paths.pkg, io::File::Mode::Read) {
    archive_size_ = pkg_.size();
    validate_index(entries_, archive_size_);
}

const Entry& Reader::entry(std::size_t index) const {
    if (index >= entries_.size())
        throw Error("file index " + nx::format("{}", index) + " out of range (archive has " +
                    nx::format("{}", entries_.size()) + ")");
    return entries_[index];
}

nx::vector<u8> Reader::read(std::size_t index) {
    const Entry& e = entry(index);
    nx::vector<u8> data(e.size);
    pkg_.read_exact_at(e.offset(), data);
    return data;
}

std::size_t Reader::read_head(std::size_t index, std::span<u8> out) {
    const Entry& e = entry(index);
    const std::size_t n = std::min<std::size_t>(out.size(), e.size);
    pkg_.read_exact_at(e.offset(), out.first(n));
    return n;
}

void Reader::read_at(std::size_t index, std::uint64_t offset, std::span<u8> out) {
    const Entry& e = entry(index);
    if (offset > e.size || out.size() > e.size - offset)
        throw Error("read past the end of file " + nx::format("{}", index));
    pkg_.read_exact_at(e.offset() + offset, out);
}

FileType Reader::sniff(std::size_t index) {
    std::array<u8, 16> head{};
    const std::size_t n = read_head(index, head);
    return pkg::sniff(std::span(head.data(), n));
}

void Reader::copy_to(std::size_t index, io::File& out) {
    const Entry& e = entry(index);
    out.copy_from(pkg_, e.offset(), e.size);
}

// ---------------------------------------------------------------------------------------------

Writer::Writer(const ArchivePaths& paths) : paths_(paths), pkg_(paths.pkg, io::File::Mode::Write) {}

Entry Writer::begin_entry(std::uint64_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max())
        throw Exception("file " + nx::format("{}", entries_.size()) + " is larger than 4 GiB");
    return Entry{next_sector_, static_cast<std::uint32_t>(size), 0};
}

void Writer::end_entry(const Entry& entry) {
    const std::uint64_t padding = std::uint64_t{entry.sector_count()} * kSectorSize - entry.size;
    pkg_.write_zeros(padding);
    const std::uint64_t next = std::uint64_t{next_sector_} + entry.sector_count();
    if (next > std::numeric_limits<std::uint32_t>::max()) throw Exception("archive exceeds the sector limit");
    next_sector_ = static_cast<std::uint32_t>(next);
    entries_.push_back(entry);
}

void Writer::add(std::span<const u8> data) {
    const Entry e = begin_entry(data.size());
    pkg_.write(data);
    end_entry(e);
}

void Writer::add_from(io::File& src, std::uint64_t offset, std::uint64_t size) {
    const Entry e = begin_entry(size);
    pkg_.copy_from(src, offset, size);
    end_entry(e);
}

void Writer::add_file(nx::fs::path_view path) {
    io::File src(path, io::File::Mode::Read);
    add_from(src, 0, src.size());
}

void Writer::finish() {
    pkg_.close();
    value_or_throw(io::write_file(paths_.fse, serialize_index(entries_)));
}

}  // namespace ff7cc::pkg
