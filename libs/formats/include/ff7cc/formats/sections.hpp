#pragma once

// Section files: archive entry 1 (loaded at boot) is a list of sections:
//   u32 count, { u32 offset, u32 size }[count], header data up to 0x80, then the sections; offsets are relative to
//   0x80 and 64-byte aligned (some on larger boundaries: 0x11000).
// Its sections include three MBD tables (3: messages such as "Load failed.", 5: "Obtained ...!", 7: the main menu's
// help lines) and the name table's fields (8). Rebuilt with a section of another size, the sections after it move by
// whole 4 KB, which keeps every alignment they had, and the list gives their new places.

#include <cstddef>
#include <span>

#include "core/foundation/core/foundation.h"
#include "ff7cc/core.hpp"

namespace ff7cc::sections {

inline constexpr std::size_t kBase = 0x80;

struct Section {
    std::size_t offset = 0, size = 0;  // in the file (kBase added)
};

nx::vector<Section> parse(std::span<const u8> file);  // throws Exception
// The file with section `index` replaced by `data`.
nx::vector<u8> replace(std::span<const u8> file, std::size_t index, std::span<const u8> data);

}  // namespace ff7cc::sections
