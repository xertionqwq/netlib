cmake_minimum_required(VERSION 3.14)
project(netlib_install_consumer LANGUAGES CXX)

# 故意从较低标准开始，验证依赖 target 能传递 C++17 要求。
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(netlib CONFIG REQUIRED)
find_package(netlib CONFIG REQUIRED)

if(DEFINED NETLIB_EXPECTED_PREFIX)
    get_filename_component(expected_prefix "${NETLIB_EXPECTED_PREFIX}" REALPATH)
    get_filename_component(package_dir "${netlib_DIR}" REALPATH)
    string(FIND "${package_dir}/" "${expected_prefix}/" prefix_position)
    if(NOT prefix_position EQUAL 0)
        message(FATAL_ERROR "netlib must be loaded from ${expected_prefix}, got ${package_dir}")
    endif()
endif()

add_executable(netlib_install_consumer main.cpp peer.cpp)
target_link_libraries(netlib_install_consumer PRIVATE netlib::netlib)

enable_testing()
add_test(NAME netlib_install_consumer COMMAND netlib_install_consumer)
set(NETLIB_INSTALL_TEST_TIMEOUT 10 CACHE STRING "Installed consumer test timeout in seconds")
set_tests_properties(netlib_install_consumer PROPERTIES TIMEOUT "${NETLIB_INSTALL_TEST_TIMEOUT}")
