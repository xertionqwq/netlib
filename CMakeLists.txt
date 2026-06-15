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

    # 已有测试
    add_executable(test_net tests/test_net.cpp)
    target_link_libraries(test_net PRIVATE netlib)
    add_test(NAME test_net COMMAND test_net)

    add_executable(test_ucontext tests/test_ucontext.cpp)
    target_link_libraries(test_ucontext PRIVATE netlib)
    add_test(NAME test_ucontext COMMAND test_ucontext)

    add_executable(test_semaphore tests/test_semaphore.cpp)
    target_link_libraries(test_semaphore PRIVATE netlib)
    add_test(NAME test_semaphore COMMAND test_semaphore)

    # 阶段一：协程核心（按依赖顺序添加）
    add_executable(test_log tests/test_log.cpp)
    target_link_libraries(test_log PRIVATE netlib)
    add_test(NAME test_log COMMAND test_log)

    add_executable(test_thread tests/test_thread.cpp)
    target_link_libraries(test_thread PRIVATE netlib)
    add_test(NAME test_thread COMMAND test_thread)

    add_executable(test_fiber tests/test_fiber.cpp)
    target_link_libraries(test_fiber PRIVATE netlib)
    add_test(NAME test_fiber COMMAND test_fiber)

    add_executable(test_scheduler tests/test_scheduler.cpp)
    target_link_libraries(test_scheduler PRIVATE netlib)
    add_test(NAME test_scheduler COMMAND test_scheduler)

    add_executable(test_timer tests/test_timer.cpp)
    target_link_libraries(test_timer PRIVATE netlib)
    add_test(NAME test_timer COMMAND test_timer)
endif()

if(NETLIB_BUILD_EXAMPLES)
    add_executable(echo_server examples/echo_server.cpp)
    target_link_libraries(echo_server PRIVATE netlib)
endif()
