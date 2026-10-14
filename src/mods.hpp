#pragma once

#include "core/foundation/platform/filesystem.h"

namespace ff7cc::game {

// The game's archive: PSP_GAME/USRDIR/discimg.pkg, read by sector through the UMD block devices at this LBN (the
// one PPSSPP assigns, so logs stay comparable), with its index in discimg.fse.
constexpr std::uint32_t kPkgLbn = 0x463;

// Maps discimg.pkg for raw disc reads. Call after rt::set_storage().
void install_archive();

// Mods: each folder in `mods_dir` (applied in name order, later ones win) may hold pkg/NNNNN.<ext> files that
// replace archive entry NNNNN. The game sees a patched discimg.fse whose replaced entries point past the end of
// discimg.pkg, and block reads there are served from the mod files. Call after install_archive().
void set_mods(nx::string_view mods_dir);

}  // namespace ff7cc::game
