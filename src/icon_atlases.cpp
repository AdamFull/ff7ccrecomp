// The game's textures with PlayStation button icons (ff7cc/gpu/button_icons.hpp): positions and texel hashes only.
// Find more with --tex-dump VBLANK DIR (files are named by the hash used here).

#include <span>

#include "ff7cc/gpu/button_icons.hpp"

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

}  // namespace ff7cc::game
