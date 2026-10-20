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

// The game's two font tables (font id -> record: regular, italic, small, small copy), each with records of its own:
// dialogue and most text use the first, the name shown above dialogue (the code around 0x088BCB88) and the menus
// (the small font) the second. Both tables' regular and italic records get the added glyphs; their small records
// those drawn in the small sheet too.
struct FontTable {
    std::uint32_t table, records[4];
};
constexpr FontTable kFontTables[] = {{0x089AD85C, {0x089AD1E8, 0x089AD384, 0x089AD520, 0x089AD6BC}},
                                     {0x089AE564, {0x089ADEF0, 0x089AE08C, 0x089AE228, 0x089AE3C4}}};
// A record: u32 glyph count, bits per texel, cells per row, cell width, cell height, sheet offset; u32 advances.
constexpr std::uint32_t kHeaderBytes = 24, kRecordBytes = 412;

// glyph index -> advance in the regular font, in the small one (0: not drawn there), from the mods (a later mod wins)
nx::map<int, std::pair<int, int>> g_advances;

}  // namespace

void collect_mod_glyphs(std::span<const nx::string> mods) {
    g_advances.clear();
    for (const auto& folder : mods) {
        const nx::string path = path_join(folder, "font/glyphs.txt");
        if (!is_file(path)) continue;
        const auto text = nx::fs::file_read_text(path);
        try {
            if (!text.has_value()) throw Exception("unreadable");
            for (const auto& g : font::parse_glyphs(text.value()))
                if (g.advance) g_advances[g.index] = {g.advance, g.small_advance};  // (aliases: the game's glyphs)
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
    for (const FontTable& t : kFontTables)
        for (std::uint32_t font = 0; font < 4; ++font)
            if (mem.read32(t.table + font * 4) != t.records[font]) {
                std::fprintf(stderr, "mods: the font tables are not as expected; added characters are not available\n");
                return;
            }
    const bool small = std::ranges::any_of(g_advances, [](const auto& a) { return a.second.second != 0; });
    for (const FontTable& t : kFontTables)
        for (std::uint32_t font = 0; font < (small ? 4u : 2u); ++font) {
            // The record with room for every glyph: header and the game's advances copied, the mods' advances added.
            const std::uint32_t record = kernel.kernel_heap().alloc(kHeaderBytes + glyphs * 4);
            if (!record) return;
            std::memcpy(mem.ptr(record), mem.ptr(t.records[font]), kRecordBytes - 4);
            for (std::uint32_t g = font::kFirstGlyphs; g < glyphs; ++g) {
                const auto it = g_advances.find(static_cast<int>(g));
                const int advance = it == g_advances.end() ? 0 : font < 2 ? it->second.first : it->second.second;
                mem.write32(record + kHeaderBytes + g * 4, static_cast<std::uint32_t>(advance));
            }
            mem.write32(record, glyphs);  // the glyph count
            mem.write32(t.table + font * 4, record);
        }
    std::fprintf(stderr, "mods: %zu added characters (glyphs up to %d)\n", g_advances.size(), last);
}

}  // namespace ff7cc::game
