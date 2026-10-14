add_executable(ccatel main.cpp)
target_link_libraries(ccatel PRIVATE ff7cc::game_formats)
psprecore_target_defaults(ccatel)
