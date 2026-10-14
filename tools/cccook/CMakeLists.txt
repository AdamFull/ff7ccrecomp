add_executable(cccook main.cpp)
target_link_libraries(cccook PRIVATE ff7cc::formats)
ff7cc_target_defaults(cccook)
