#pragma once

// Characters mods add to the regular font (ff7cc/formats/font.hpp, docs/text_rendering.md). Their shapes come with
// the mod's font sheet (pkg/01472.bin, from `cctext font-import`); their advances, from font/glyphs.txt, go into
// larger copies of the regular and italic font records that the game's font table is pointed at.

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::game {

void collect_mod_glyphs(nx::string_view mods_dir);  // when the disc is mounted
void install_mod_glyphs();                          // once the game is in memory, before it runs

}  // namespace ff7cc::game
