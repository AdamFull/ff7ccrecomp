// ff7cc: Crisis Core -Final Fantasy VII- (ULES-01044) on the recompiled-game runtime. Options: see the runtime's
// frontend (psprecore/platform/frontend.hpp).

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, called by SDLActivity
#endif

#include "psprecore/gpu/button_icons.hpp"
#include "psprecore/gpu/ui_font.hpp"
#include "psprecore/platform/frontend.hpp"
#include "psprecore/runtime/system.hpp"
#include "fonts.hpp"
#include "mods.hpp"

namespace ff7cc::game {

extern const std::span<const rt::gpu::IconAtlas> kIconAtlases;
rt::gpu::UiFont ui_font();

// Movies the player cannot skip ("sceMpeg: movie <key>" in the log): the title screen plays over this one, and it
// ending without input starts the logo loop again.
constexpr std::uint64_t kUnskippableMovies[] = {0x001A60EC489C619Dull};

namespace {

void mount(rt::System& system, std::span<const nx::string> mods) {
    install_archive(system.storage);
    set_mods(system.storage, mods);
    collect_mod_glyphs(mods);
}

constexpr platform::GameInfo kGame{
    .title = "Crisis Core -Final Fantasy VII-",
    .name = "ff7cc",
    .folder = "Crisis Core - Final Fantasy VII EU",
    .load_base = 0x08804000,
    .mount = mount,
    .loaded = install_mod_glyphs,
    .unskippable_movies = kUnskippableMovies,
};

}  // namespace

}  // namespace ff7cc::game

int main(int argc, char** argv) {
    psprecore::rt::gpu::set_icon_atlases(ff7cc::game::kIconAtlases);
    psprecore::rt::gpu::set_ui_font(ff7cc::game::ui_font());
    return psprecore::platform::run_game(ff7cc::game::kGame, argc, argv);
}
