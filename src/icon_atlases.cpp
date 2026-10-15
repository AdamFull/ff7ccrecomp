// The game's textures with PlayStation button icons (psprecore/gpu/button_icons.hpp) and its text font
// (psprecore/gpu/ui_font.hpp): positions, cell layout and texel hashes only. Find more with --tex-dump VBLANK:DIR
// (files are named by the hash used here).

#include <span>

#include "ff7cc/core.hpp"
#include "ff7cc/formats/mbd.hpp"
#include "psprecore/gpu/button_icons.hpp"
#include "psprecore/gpu/ui_font.hpp"

namespace ff7cc::game {

namespace {

using rt::gpu::Icon;
using rt::gpu::IconAtlas;
using enum rt::gpu::IconFace;

// Battle HUD atlas: Cross, Circle, Triangle, Square in a row (11x11 discs).
constexpr Icon kBattleHud[] = {{5, 101, Cross}, {16, 101, Circle}, {27, 101, Triangle}, {38, 101, Square}};
// Text font sheet: Triangle after the regular and the italic characters (13x13 discs).
constexpr Icon kFontSheet[] = {{6, 76, Triangle}, {6, 160, Triangle}};

constexpr IconAtlas kAtlases[] = {
    {5, 256, 256, 0x113A6D124D4EBA4Cull, kBattleHud},
    {4, 256, 256, 0x995C582AE01F4C31ull, kFontSheet},
};

}  // namespace

extern const std::span<const IconAtlas> kIconAtlases = kAtlases;

// The text font: 13x14 cells, 19 to a row (glyph n in column n % 19, row n / 19), the characters cctext maps
// (ff7cc/formats/mbd.hpp); its Triangle icon lends its disc to the prompts the port adds.
rt::gpu::UiFont ui_font() {
    rt::gpu::UiFont font;
    font.atlas_hash = kAtlases[1].hash;
    font.cell_width = 13;
    font.cell_height = 14;
    font.columns = 19;
    font.ascii.fill(-1);
    const mbd::Charset charset = mbd::Charset::european();
    for (std::size_t g = 0; g < charset.glyphs.size(); ++g)
        if (const nx::string& c = charset.glyphs[g]; c.size() == 1 && static_cast<unsigned char>(c[0]) < 128)
            font.ascii[static_cast<unsigned char>(c[0])] = static_cast<std::int16_t>(g);
    font.button = kFontSheet[0];
    // Loaded at boot, long before the first subtitle: swizzled 4-bit indices, the palette (8888) in CCFF7ENUK's data.
    font.texels = 0x09EC7C60;
    font.palette = 0x08A2B3C0;
    font.format = kAtlases[1].format;
    font.width = kAtlases[1].width;
    font.height = kAtlases[1].height;
    font.buffer_width = 256;
    font.swizzled = true;
    font.palette_format = 3;
    return font;
}

}  // namespace ff7cc::game
