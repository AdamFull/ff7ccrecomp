add_executable(cctext main.cpp)
target_link_libraries(cctext PRIVATE ff7cc::formats)
ff7cc_target_defaults(cctext)
