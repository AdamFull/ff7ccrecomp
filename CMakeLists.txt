cmake_minimum_required(VERSION 3.25)

project(FF7CCRECOMP
    VERSION 0.1.0
    DESCRIPTION "Native port of Crisis Core: Final Fantasy VII (PSP) via static recompilation"
    LANGUAGES C CXX)

# The recompiler and runtime (psprecore): libraries, front end, tools and their tests.
add_subdirectory(psprecore)

add_subdirectory(libs/formats)

if(PSPRECORE_BUILD_TOOLS)
    add_subdirectory(tools/ccpkg)
    add_subdirectory(tools/ccatel)
    add_subdirectory(tools/cccook)
    add_subdirectory(tools/cctext)
    add_subdirectory(tools/cctex)

    # Edited textures (assets/textures, laid out by cctex unpack) are packed into the cooked media by every build
    # (psptex pack compares the pictures' pixels with the originals, so it is quick, and file times do not matter:
    # a picture restored from a backup keeps its old time). assets/textures-hd (psptex match, e.g. from another
    # project's texture pack) fills in where a picture in assets/textures is unedited.
    set(FF7CC_COOKED_DIR "${PROJECT_SOURCE_DIR}/data/cooked" CACHE PATH "cooked media the game loads")
    set(_ff7cc_textures "${PROJECT_SOURCE_DIR}/assets/textures")
    if(EXISTS "${_ff7cc_textures}/index.tsv")
        add_custom_target(ff7cc_textures ALL
            COMMAND psptex pack "${_ff7cc_textures}" "${FF7CC_COOKED_DIR}"
                    --layer "${PROJECT_SOURCE_DIR}/assets/textures-hd"
            DEPENDS psptex
            COMMENT "Packing edited textures (assets/textures, assets/textures-hd -> cooked/textures)"
            VERBATIM)
    endif()
endif()

if(PSPRECORE_BUILD_TESTS)
    enable_testing()  # here too, so ctest from the build root runs psprecore's tests and the game's
    add_subdirectory(tests)
endif()

# The game needs code generated from the user's own BOOT.BIN (psprecomp -> data/generated); src/decomp holds
# hand-written replacements for recompiled functions (GAME_FUNC; run psprecomp with --overrides src/decomp).
set(FF7CC_GENERATED_DIR "${PROJECT_SOURCE_DIR}/data/generated" CACHE PATH "psprecomp output directory")
if(PSPRECORE_BUILD_GAME)
    file(GLOB _ff7cc_game_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
    psprecore_add_game(ff7cc
        GENERATED_DIR "${FF7CC_GENERATED_DIR}"
        OVERRIDES_DIR "${PROJECT_SOURCE_DIR}/src/decomp"
        SOURCES ${_ff7cc_game_sources})
    if(TARGET ff7cc)
        target_link_libraries(ff7cc PRIVATE ff7cc::game_formats)
    endif()
endif()
