#include "fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ff7cc/core.hpp"
#include "ff7cc/formats/font.hpp"
#include "psprecore/error.hpp"
#include "psprecore/io/path.hpp"
#include "psprecore/runtime/system.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/platform/filesystem.h"

namespace ff7cc::game {

namespace {

// The game's font table (font id -> record) and the records of the regular and italic fonts it holds.
constexpr std::uint32_t kFontTable = 0x089AD85C;
constexpr std::uint32_t kFontRecords[2] = {0x089AD1E8, 0x089AD384};
// A record: u32 glyph count, bits per texel, cells per row, cell width, cell height, sheet offset; u32 advances.
constexpr std::uint32_t kHeaderBytes = 24, kRecordBytes = 412;

nx::map<int, int> g_advances;  // glyph index -> advance, from the mods (a later mod wins)

}  // namespace

void collect_mod_glyphs(std::span<const nx::string> mods) {
    g_advances.clear();
    for (const auto& folder : mods) {
        const nx::string path = path_join(folder, "font/glyphs.txt");
        if (!is_file(path)) continue;
        const auto text = nx::fs::file_read_text(path);
        try {
            if (!text.has_value()) throw Exception("unreadable");
            for (const auto& g : font::parse_glyphs(text.value())) g_advances[g.index] = g.advance;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "mods: %s: %s\n", path.c_str(), e.what());
        }
    }
}

void install_mod_glyphs(rt::System& system) {
    if (g_advances.empty()) return;
    rt::Kernel& kernel = system.kernel;
    rt::GuestMemory& mem = system.memory;
    int last = font::kFirstGlyphs - 1;
    for (const auto& [index, _] : g_advances) last = std::max(last, index);
    const auto glyphs = static_cast<std::uint32_t>(last + 1);
    for (std::uint32_t font = 0; font < 2; ++font) {
        if (mem.read32(kFontTable + font * 4) != kFontRecords[font]) {
            std::fprintf(stderr, "mods: the font table is not as expected; added characters are not available\n");
            return;
        }
        // The record with room for every glyph: header and the game's advances copied, the mods' advances added.
        const std::uint32_t record = kernel.kernel_heap().alloc(kHeaderBytes + glyphs * 4);
        if (!record) return;
        std::memcpy(mem.ptr(record), mem.ptr(kFontRecords[font]), kRecordBytes - 4);
        for (std::uint32_t g = font::kFirstGlyphs; g < glyphs; ++g) {
            const auto it = g_advances.find(static_cast<int>(g));
            mem.write32(record + kHeaderBytes + g * 4, it == g_advances.end() ? 0 : static_cast<std::uint32_t>(it->second));
        }
        mem.write32(record, glyphs);  // the glyph count
        mem.write32(kFontTable + font * 4, record);
    }
    std::fprintf(stderr, "mods: %zu added characters (glyphs up to %d)\n", g_advances.size(), last);
}

}  // namespace ff7cc::game
