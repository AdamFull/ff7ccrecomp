add_executable(cctext main.cpp)
target_link_libraries(cctext PRIVATE ff7cc::game_formats)
psprecore_target_defaults(cctext)
