# Tests of the game's own formats (the test runner comes from the core).
add_executable(ff7cc_game_tests
    ${PSPRECORE_DIR}/tests/test_main.cpp
    pkg_tests.cpp
    atel_tests.cpp
    atel_vm_tests.cpp
    atel_asm_tests.cpp
)
target_link_libraries(ff7cc_game_tests PRIVATE ff7cc::game_formats nx::foundation)
target_include_directories(ff7cc_game_tests PRIVATE ${PSPRECORE_DIR}/tests)
psprecore_target_defaults(ff7cc_game_tests)

add_test(NAME ff7cc_game_tests COMMAND ff7cc_game_tests)
