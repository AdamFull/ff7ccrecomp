# Crisis Core's own formats: the archive (discimg.pkg/fse), ATEL scripts, MBD message data.
add_library(ff7cc_game_formats STATIC
    src/formats/pkg.cpp
    src/formats/atel.cpp
    src/formats/atel_vm.cpp
    src/formats/atel_disasm.cpp
    src/formats/atel_asm.cpp
    src/formats/mbd.cpp
)
add_library(ff7cc::game_formats ALIAS ff7cc_game_formats)

target_include_directories(ff7cc_game_formats PUBLIC include)
target_link_libraries(ff7cc_game_formats PUBLIC ff7cc::formats)
ff7cc_target_defaults(ff7cc_game_formats)
