add_executable(ccpkg main.cpp)
target_link_libraries(ccpkg PRIVATE ff7cc::game_formats)
ff7cc_target_defaults(ccpkg)
