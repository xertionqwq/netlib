cmake_minimum_required(VERSION 3.14)
project(netlib LANGUAGES CXX)

# netlib — header-only epoll + reactor network library
# C++17, zero dependencies beyond the STL

add_library(netlib INTERFACE)
target_include_directories(netlib INTERFACE include)
target_compile_features(netlib INTERFACE cxx_std_17)

option(NETLIB_BUILD_EXAMPLES "Build example programs" OFF)
option(NETLIB_BUILD_TESTS "Build tests" OFF)

if(NETLIB_BUILD_TESTS)
    enable_testing()
    add_executable(test_net tests/test_net.cpp)
    target_link_libraries(test_net PRIVATE netlib)
    add_test(NAME test_net COMMAND test_net)
endif()

if(NETLIB_BUILD_EXAMPLES)
    add_executable(echo_server examples/echo_server.cpp)
    target_link_libraries(echo_server PRIVATE netlib)
endif()
