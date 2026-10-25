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

    foreach(buffer_target IN ITEMS test_buffer test_buffer_ndebug)
        add_executable(${buffer_target} tests/test_buffer.cpp tests/test_buffer_peer.cpp)
        target_link_libraries(${buffer_target} PRIVATE netlib)
        target_link_options(${buffer_target} PRIVATE "-Wl,--wrap=write")
        add_test(NAME ${buffer_target} COMMAND ${buffer_target})
        set_tests_properties(${buffer_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_buffer_ndebug PRIVATE NDEBUG)

    foreach(connection_target IN ITEMS test_tcpconnection test_tcpconnection_ndebug)
        add_executable(${connection_target} tests/test_tcpconnection.cpp tests/test_tcpconnection_peer.cpp)
        target_link_libraries(${connection_target} PRIVATE netlib)
        target_link_options(${connection_target} PRIVATE
            "-Wl,--wrap=readv" "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
        add_test(NAME ${connection_target} COMMAND ${connection_target})
        set_tests_properties(${connection_target} PROPERTIES TIMEOUT 20)
    endforeach()
    target_compile_definitions(test_tcpconnection_ndebug PRIVATE NDEBUG)

    foreach(server_target IN ITEMS test_tcpserver test_tcpserver_ndebug)
        add_executable(${server_target} tests/test_tcpserver.cpp tests/test_tcpserver_peer.cpp)
        target_link_libraries(${server_target} PRIVATE netlib)
        target_link_options(${server_target} PRIVATE "-Wl,--wrap=socket" "-Wl,--wrap=accept4"
            "-Wl,--wrap=getsockname" "-Wl,--wrap=epoll_ctl" "-Wl,--wrap=close")
        add_test(NAME ${server_target} COMMAND ${server_target})
        set_tests_properties(${server_target} PROPERTIES TIMEOUT 25)
    endforeach()
    target_compile_definitions(test_tcpserver_ndebug PRIVATE NDEBUG)

    foreach(socket_target IN ITEMS test_socket test_socket_ndebug)
        add_executable(${socket_target} tests/test_socket.cpp tests/test_socket_peer.cpp)
        target_link_libraries(${socket_target} PRIVATE netlib)
        add_test(NAME ${socket_target} COMMAND ${socket_target})
        set_tests_properties(${socket_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_socket_ndebug PRIVATE NDEBUG)

    foreach(acceptor_target IN ITEMS test_acceptor test_acceptor_ndebug)
        add_executable(${acceptor_target} tests/test_acceptor.cpp tests/test_acceptor_peer.cpp)
        target_link_libraries(${acceptor_target} PRIVATE netlib)
        target_link_options(${acceptor_target} PRIVATE "-Wl,--wrap=socket" "-Wl,--wrap=accept4" "-Wl,--wrap=epoll_ctl")
        add_test(NAME ${acceptor_target} COMMAND ${acceptor_target})
        set_tests_properties(${acceptor_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_acceptor_ndebug PRIVATE NDEBUG)

    add_executable(test_channel tests/test_channel.cpp tests/test_channel_peer.cpp)
    target_link_libraries(test_channel PRIVATE netlib)
    add_test(NAME test_channel COMMAND test_channel)

    add_executable(test_channel_ndebug tests/test_channel.cpp tests/test_channel_peer.cpp)
    target_link_libraries(test_channel_ndebug PRIVATE netlib)
    target_compile_definitions(test_channel_ndebug PRIVATE NDEBUG)
    add_test(NAME test_channel_ndebug COMMAND test_channel_ndebug)
    set_tests_properties(test_channel test_channel_ndebug PROPERTIES TIMEOUT 30)

    # EPollPoller 使用真实后端，只对 epoll_wait 的异常路径进行一次性故障注入。
    foreach(poller_target IN ITEMS test_epollpoller test_epollpoller_ndebug)
        add_executable(${poller_target} tests/test_epollpoller.cpp)
        target_link_libraries(${poller_target} PRIVATE netlib)
        target_link_options(${poller_target} PRIVATE "-Wl,--wrap=epoll_wait")
        add_test(NAME ${poller_target} COMMAND ${poller_target})
        set_tests_properties(${poller_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_epollpoller_ndebug PRIVATE NDEBUG)

    # EventLoop 独立测试使用 tests/support 的 poll(2) 后端进行故障注入。
    foreach(eventloop_target IN ITEMS test_eventloop test_eventloop_ndebug)
        add_executable(${eventloop_target} tests/test_eventloop.cpp tests/test_eventloop_peer.cpp)
        target_include_directories(${eventloop_target} BEFORE PRIVATE tests/support)
        target_link_libraries(${eventloop_target} PRIVATE netlib)
        add_test(NAME ${eventloop_target} COMMAND ${eventloop_target})
        set_tests_properties(${eventloop_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_eventloop_ndebug PRIVATE NDEBUG)

    foreach(loopthread_target IN ITEMS test_eventloopthread test_eventloopthread_ndebug)
        add_executable(${loopthread_target} tests/test_eventloopthread.cpp tests/test_eventloopthread_peer.cpp)
        target_link_libraries(${loopthread_target} PRIVATE netlib)
        add_test(NAME ${loopthread_target} COMMAND ${loopthread_target})
        set_tests_properties(${loopthread_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_eventloopthread_ndebug PRIVATE NDEBUG)

    foreach(loopthreadpool_target IN ITEMS test_eventloopthreadpool test_eventloopthreadpool_ndebug)
        add_executable(${loopthreadpool_target} tests/test_eventloopthreadpool.cpp tests/test_eventloopthreadpool_peer.cpp)
        target_link_libraries(${loopthreadpool_target} PRIVATE netlib)
        add_test(NAME ${loopthreadpool_target} COMMAND ${loopthreadpool_target})
        set_tests_properties(${loopthreadpool_target} PROPERTIES TIMEOUT 15)
    endforeach()
    target_compile_definitions(test_eventloopthreadpool_ndebug PRIVATE NDEBUG)

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

    # 同一套日志测试在禁用标准 assert 时仍须验证 FATAL 和项目断言
    add_executable(test_log_ndebug tests/test_log.cpp)
    target_link_libraries(test_log_ndebug PRIVATE netlib)
    target_compile_definitions(test_log_ndebug PRIVATE NDEBUG)
    add_test(NAME test_log_ndebug COMMAND test_log_ndebug)
    set_tests_properties(test_log test_log_ndebug PROPERTIES TIMEOUT 30)

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
