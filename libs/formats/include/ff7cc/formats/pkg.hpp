#pragma once

// discimg.fse / discimg.pkg — the game's main data archive.
//
// discimg.fse is a flat array of 12-byte little-endian entries:
//     u32 sector    start of the file in 2048-byte sectors
//     u32 size      size in bytes
//     u32 reserved  always 0 on the EU disc
// Files are unnamed and addressed by index. They are stored contiguously, each
// padded to a sector boundary. Entry 0 is the archive's own "Atel" header.
// On the original disc the padding is random filler; this code writes zeros.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "ff7cc/io/file.hpp"
#include "core/foundation/core/foundation.h"

namespace ff7cc::pkg {

inline constexpr std::uint32_t kSectorSize = 0x800;
inline constexpr std::size_t kEntrySize = 12;

struct Entry {
    std::uint32_t sector = 0;
    std::uint32_t size = 0;
    std::uint32_t reserved = 0;

    std::uint64_t offset() const { return std::uint64_t{sector} * kSectorSize; }
    std::uint64_t end() const { return offset() + size; }
    std::uint32_t sector_count() const { return static_cast<std::uint32_t>((std::uint64_t{size} + kSectorSize - 1) / kSectorSize); }

    friend bool operator==(const Entry&, const Entry&) = default;
};

nx::vector<Entry> parse_index(std::span<const std::byte> fse);
nx::vector<std::byte> serialize_index(std::span<const Entry> entries);

// Throws FormatError if entries are out of order, overlap, or run past archive_size.
void validate_index(std::span<const Entry> entries, std::uint64_t archive_size);

enum class FileType : std::uint8_t { Empty, Atel, Riff, Psmf, Sscf, Unknown };

// Identifies a file from its first bytes (pass at least 16 when available).
FileType sniff(std::span<const std::byte> head);
std::string_view type_name(FileType type);       // e.g. "atel"
std::string_view type_extension(FileType type);  // e.g. ".atel"
// Inverse of type_name(); returns Unknown for unrecognised names.
FileType type_from_name(std::string_view name);

struct ArchivePaths {
    std::filesystem::path fse;
    std::filesystem::path pkg;
};

// Finds discimg.fse/pkg given a disc root, PSP_GAME, USRDIR, or the directory holding them.
ArchivePaths locate(const std::filesystem::path& dir);
// Paths for writing a new archive into `dir`.
ArchivePaths output_paths(const std::filesystem::path& dir);

class Reader {
public:
    explicit Reader(const ArchivePaths& paths);

    std::size_t size() const { return entries_.size(); }
    std::span<const Entry> entries() const { return entries_; }
    const Entry& entry(std::size_t index) const;
    std::uint64_t archive_size() const { return archive_size_; }

    nx::vector<std::byte> read(std::size_t index);
    // Reads min(out.size(), entry size) bytes from the start of the file; returns the count.
    std::size_t read_head(std::size_t index, std::span<std::byte> out);
    // Reads exactly out.size() bytes starting `offset` bytes into the file.
    void read_at(std::size_t index, std::uint64_t offset, std::span<std::byte> out);
    FileType sniff(std::size_t index);
    // Streams the file's contents to the current position of `out`.
    void copy_to(std::size_t index, io::File& out);

private:
    nx::vector<Entry> entries_;
    io::File pkg_;
    std::uint64_t archive_size_ = 0;
};

class Writer {
public:
    explicit Writer(const ArchivePaths& paths);

    // Appends the next file (index = number of files added so far).
    void add(std::span<const std::byte> data);
    void add_from(io::File& src, std::uint64_t offset, std::uint64_t size);
    void add_file(const std::filesystem::path& path);
    // Writes the index and closes both files. Must be called once all files are added.
    void finish();

    std::size_t size() const { return entries_.size(); }

private:
    Entry begin_entry(std::uint64_t size);
    void end_entry(const Entry& entry);

    ArchivePaths paths_;
    io::File pkg_;
    nx::vector<Entry> entries_;
    std::uint32_t next_sector_ = 0;
};

}  // namespace ff7cc::pkg
