// ff7cc: Crisis Core -Final Fantasy VII- (ULES-01044) on the recompiled-game runtime. Options: see the runtime's
// frontend (ff7cc/platform/frontend.hpp).

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, called by SDLActivity
#endif

#include "ff7cc/gpu/button_icons.hpp"
#include "ff7cc/platform/frontend.hpp"
#include "mods.hpp"

namespace ff7cc::game {

extern const std::span<const rt::gpu::IconAtlas> kIconAtlases;

namespace {

void mount(nx::string_view mods_dir) {
    install_archive();
    set_mods(mods_dir);
}

constexpr platform::GameInfo kGame{
    .title = "Crisis Core -Final Fantasy VII-",
    .name = "ff7cc",
    .folder = "Crisis Core - Final Fantasy VII EU",
    .load_base = 0x08804000,
    .mount = mount,
};

}  // namespace

}  // namespace ff7cc::game

int main(int argc, char** argv) {
    ff7cc::rt::gpu::set_icon_atlases(ff7cc::game::kIconAtlases);
    return ff7cc::platform::run_game(ff7cc::game::kGame, argc, argv);
}
