add_executable(cctext main.cpp png.cpp)
target_link_libraries(cctext PRIVATE ff7cc::game_formats psprecore_stb)
psprecore_target_defaults(cctext)
