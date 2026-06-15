#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "netlib/log.h"
#include "netlib/timer.h"

// ============================================================
// Timer 测试 — 毫秒级定时器基础功能
// ============================================================

// 1. 定时器到期后回调被执行
void test_timer_expires() {
    // TODO: 等 Timer 实现后补全
    // netlib::TimerManager mgr;
    // bool called = false;
    // mgr.addTimer(std::make_shared<netlib::Timer>(100, [&]() { called = true; }));
    // std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // auto cbs = mgr.listExpiredCb();
    // assert(cbs.size() == 1);
    // cbs[0]();
    // assert(called);
}

// 2. 未到期定时器不会触发
void test_timer_not_expired() {
    // TODO: 定时器设 500ms，只等 100ms，listExpiredCb 应为空
}

// 3. 多个定时器按到期顺序排列
void test_multiple_timers() {
    // TODO: 3 个定时器 (200ms, 100ms, 300ms)，到期顺序应为 100, 200, 300
}

// 4. 循环定时器到期后自动重新加入
void test_recurring_timer() {
    // TODO: 循环定时器隔 50ms，list 3 次，每次都有该 timer
}

// 5. getNextTimer 返回最近到期时间
void test_get_next_timer() {
    // TODO: 定时器 300ms 后到期，getNextTimer() 应返回约 300
}

int main() {
    // 等 Timer 实现后逐一取消注释
    std::cout << "test_timer: skeleton ready (TODO: implement Timer first)"
              << std::endl;
    return 0;
}
