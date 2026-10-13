add_executable(ccpkg main.cpp)
target_link_libraries(ccpkg PRIVATE ff7cc::formats)
ff7cc_target_defaults(ccpkg)
