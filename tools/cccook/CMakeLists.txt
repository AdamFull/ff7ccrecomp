add_executable(cccook main.cpp)
target_link_libraries(cccook PRIVATE ff7cc::game_formats)
psprecore_target_defaults(cccook)
