#pragma once

#include <span>

#include "core/foundation/platform/filesystem.h"
#include "ff7cc/core.hpp"

namespace ff7cc::game {

// The game's archive: PSP_GAME/USRDIR/discimg.pkg, read by sector through the UMD block devices at this LBN (the
// one PPSSPP assigns, so logs stay comparable), with its index in discimg.fse.
constexpr std::uint32_t kPkgLbn = 0x463;

// Maps discimg.pkg for raw disc reads. Call after rt::set_storage().
void install_archive();

// Mods: each active mod folder (in order, later ones win; psprecore/platform/mods.hpp) may hold pkg/NNNNN.<ext>
// files that replace archive entry NNNNN. The game sees a patched discimg.fse whose replaced entries point past the
// end of discimg.pkg, and block reads there are served from the mod files. Call after install_archive().
void set_mods(std::span<const nx::string> mods);

}  // namespace ff7cc::game
