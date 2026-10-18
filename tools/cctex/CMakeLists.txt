add_executable(cctex main.cpp)
target_link_libraries(cctex PRIVATE ff7cc::game_formats)
psprecore_target_defaults(cctex)
