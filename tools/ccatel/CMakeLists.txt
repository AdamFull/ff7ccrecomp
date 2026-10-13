add_executable(ccatel main.cpp)
target_link_libraries(ccatel PRIVATE ff7cc::formats)
ff7cc_target_defaults(ccatel)
