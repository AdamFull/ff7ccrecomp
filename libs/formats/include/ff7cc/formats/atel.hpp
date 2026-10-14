#pragma once

// "Atel" script modules — the engine's script VM container (543 files in discimg.pkg).
//
// Layout (little-endian):
//   +0x00 "Atel"
//   +0x04 u8 version_major (1), u8 version_minor (3), u8 reserved (0), u8 flags
//   +0x08 u32 object_count
//   +0x0C u32 names_size
//   +0x10 u32 code_size
//   +0x14 u32 seed        build timestamp; also seeds the name-table cipher
//   +0x18 objects[object_count]:
//           u16 method_count, u16 attributes, u32 name, u32 unknown,
//           { u32 name, u32 code_offset }[method_count]
//   names[names_size]     NUL-terminated symbol/string pool; "name" fields are offsets into it.
//                         XOR-obfuscated with the game LCG unless flags & kFlagPlainNames.
//   code[code_size]       VM bytecode (plain); code_offset fields point into it.
//   filler up to the next 16-byte file offset
//   u32 resource_count, u32 resource_count (repeated)
//   { u32 offset, u32 size }[resource_count]   offsets relative to the end of this table
//   resource data, each at a 16-byte aligned offset; the file ends 16-byte aligned.
//
// The bytes between a resource's declared end and the next resource ("slack") are mostly random
// filler but sometimes hold data the declared size omits (e.g. an MBD end marker), so they are
// kept verbatim. Parsing then serializing an unmodified module reproduces it byte for byte.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

namespace ff7cc::atel {

inline constexpr std::uint8_t kFlagPlainNames = 0x01;
inline constexpr std::size_t kHeaderSize = 0x18;
inline constexpr std::size_t kResourceAlignment = 16;

struct Method {
    std::uint32_t name = 0;
    std::uint32_t code_offset = 0;

    friend bool operator==(const Method&, const Method&) = default;
};

struct Object {
    std::uint16_t attributes = 1;
    std::uint32_t name = 0;
    std::uint32_t unknown = 0;
    nx::vector<Method> methods;

    friend bool operator==(const Object&, const Object&) = default;
};

struct Resource {
    nx::vector<std::byte> data;
    nx::vector<std::byte> slack;  // bytes after `data` up to the next resource (or end of file)

    friend bool operator==(const Resource&, const Resource&) = default;
};

struct Module {
    std::uint8_t version_major = 1;
    std::uint8_t version_minor = 3;
    std::uint8_t reserved = 0;
    std::uint8_t flags = 0x02;
    std::uint32_t seed = 0;
    nx::vector<Object> objects;
    nx::vector<std::byte> names;         // always decrypted in memory
    nx::vector<std::byte> code;
    nx::vector<std::byte> code_padding;  // filler between the code and the resource table
    nx::vector<Resource> resources;

    // NUL-terminated string at `offset` in the name pool (Shift-JIS for non-ASCII text).
    std::string_view name_at(std::uint32_t offset) const;

    friend bool operator==(const Module&, const Module&) = default;
};

// True if `data` starts with the Atel magic.
bool is_atel(std::span<const std::byte> data);

Module parse(std::span<const std::byte> data);
nx::vector<std::byte> serialize(const Module& module);

// Replaces resources while every other byte of the module stays where it was (the game may address resources by
// their offsets): a replacement that fits in the old resource's space (its data and slack) is written in place,
// a larger one is appended at the end of the module. Only the resource table entries of replaced resources change.
nx::vector<std::byte> replace_resources(std::span<const std::byte> module,
                                         const nx::map<std::size_t, nx::vector<std::byte>>& replacements);

// The name-table cipher (symmetric): XOR with the low byte of successive game-LCG outputs,
// state = state * 0x41C64E6D + 1234, output = (state >> 16) & 0x7FFF, seeded with Module::seed.
void crypt_names(std::span<std::byte> names, std::uint32_t seed);

// Short type tag for a resource from its leading bytes (e.g. "mbd", "sscf"); "bin" if unknown.
std::string_view resource_type(std::span<const std::byte> data);

}  // namespace ff7cc::atel
