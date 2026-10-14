cmake_minimum_required(VERSION 3.25)

project(FF7CCRECOMP
    VERSION 0.1.0
    DESCRIPTION "Native port of Crisis Core: Final Fantasy VII (PSP) via static recompilation"
    LANGUAGES C CXX)

# The recompiler and runtime (psprecore): libraries, front end, tools and their tests.
add_subdirectory(psprecore)

add_subdirectory(libs/formats)

if(FF7CC_BUILD_TOOLS)
    add_subdirectory(tools/ccpkg)
    add_subdirectory(tools/ccatel)
    add_subdirectory(tools/cccook)
    add_subdirectory(tools/cctext)
endif()

if(FF7CC_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The game needs code generated from the user's own BOOT.BIN (ccrecomp -> generated/); game/ holds hand-written
# replacements for recompiled functions (GAME_FUNC; run ccrecomp with --overrides game).
set(FF7CC_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "ccrecomp output directory")
if(FF7CC_BUILD_GAME)
    file(GLOB _ff7cc_game_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
    ff7cc_add_game(ff7cc
        GENERATED_DIR "${FF7CC_GENERATED_DIR}"
        OVERRIDES_DIR "${PROJECT_SOURCE_DIR}/game"
        SOURCES ${_ff7cc_game_sources})
    if(TARGET ff7cc)
        target_link_libraries(ff7cc PRIVATE ff7cc::game_formats)
    endif()
endif()
