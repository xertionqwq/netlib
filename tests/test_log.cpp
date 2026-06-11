#include <cassert>
#include <iostream>
#include <sstream>

#include "netlib/log.h"

// ============================================================
// Log 测试
// ============================================================

void test_log_basic() {
    NETLIB_LOG_DEBUG << "debug message";
    NETLIB_LOG_INFO  << "info message";
    NETLIB_LOG_WARN  << "warning message";
    NETLIB_LOG_ERROR << "error message";
    // 预期：无崩溃，stderr 输出格式正确
}

void test_log_assert() {
    NETLIB_ASSERT(true && "此断言不应触发");
    // 预期：不终止
}

void test_log_level_filter() {
    auto oldLevel = netlib::Logger::getLevel();
    netlib::Logger::setLevel(netlib::LogLevel::WARN);
    // DEBUG/INFO 应被过滤掉
    NETLIB_LOG_DEBUG << "should not appear";
    NETLIB_LOG_INFO  << "should not appear";
    NETLIB_LOG_WARN  << "should appear";
    netlib::Logger::setLevel(oldLevel);
}

int main() {
    test_log_basic();
    std::cout << "[1/3] test_log_basic passed" << std::endl;

    test_log_assert();
    std::cout << "[2/3] test_log_assert passed" << std::endl;

    test_log_level_filter();
    std::cout << "[3/3] test_log_level_filter passed" << std::endl;

    std::cout << "test_log: all passed" << std::endl;
    return 0;
}
