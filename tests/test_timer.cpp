#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "netlib/log.h"
#include "netlib/timer.h"

// ============================================================
// Timer 测试 — 毫秒级定时器基础功能
// ============================================================

// 1. 定时器到期后回调被取出
void test_timer_expires() {
    netlib::TimerManager mgr;
    bool called = false;

    mgr.addTimer(50, [&]() { called = true; });
    assert(mgr.hasTimer());

    // 等定时器到期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);

    assert(cbs.size() == 1);
    cbs[0]();  // 执行到期回调
    assert(called);
    assert(!mgr.hasTimer());
}

// 2. 未到期定时器不触发
void test_timer_not_expired() {
    netlib::TimerManager mgr;
    mgr.addTimer(500, []() {});

    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);

    assert(cbs.empty());
    assert(mgr.hasTimer());
}

// 3. 多个定时器按到期顺序取出
void test_multiple_timers() {
    netlib::TimerManager mgr;
    std::string trace;

    // 按 (到期时间, 标记) 添加，到期顺序: 100 < 200 < 300
    mgr.addTimer(200, [&]() { trace += "B"; });
    mgr.addTimer(100, [&]() { trace += "A"; });
    mgr.addTimer(300, [&]() { trace += "C"; });

    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);

    for (auto &cb : cbs) cb();
    assert(trace == "ABC");
}

// 4. 循环定时器到期后自动续期
void test_recurring_timer() {
    netlib::TimerManager mgr;
    int count = 0;

    mgr.addTimer(50, [&]() { count++; }, true);

    // 等 3 个间隔
    for (int i = 0; i < 3; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        std::vector<netlib::TimerManager::FunCallback> cbs;
        mgr.listExpiredCb(cbs);
        for (auto &cb : cbs) cb();
    }

    assert(count == 3);
    assert(mgr.hasTimer());  // 还在循环
}

// 5. getNextTimer 返回最近到期时间
void test_get_next_timer() {
    netlib::TimerManager mgr;
    mgr.addTimer(300, []() {});

    auto next = mgr.getNextTimer();
    // 应大约为 300ms，考虑调度误差允许 [200, 310]
    assert(next >= 200 && next <= 310);
}

// 6. cancel 取消定时器
void test_cancel() {
    netlib::TimerManager mgr;
    auto timer = mgr.addTimer(50, []() {});

    bool ok = timer->cancel();
    assert(ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);
    assert(cbs.empty());

    // 重复 cancel 应返回 false
    assert(!timer->cancel());
}

// 7. refresh 刷新定时器延长超时
void test_refresh() {
    netlib::TimerManager mgr;
    auto timer = mgr.addTimer(50, []() {});

    // 立刻刷新，等 100ms 后应只触发一次（刷新的那次）
    bool ok = timer->refresh();
    assert(ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);
    assert(cbs.size() == 1);  // 刷新后的触发
}

// 8. reset 重设定时间隔
void test_reset() {
    netlib::TimerManager mgr;
    int value = 0;
    auto timer = mgr.addTimer(200, [&]() { value = 42; });

    // 重设为 50ms
    timer->reset(50, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<netlib::TimerManager::FunCallback> cbs;
    mgr.listExpiredCb(cbs);
    assert(cbs.size() == 1);
    cbs[0]();
    assert(value == 42);
}

int main() {
    test_timer_expires();
    std::cout << "[1/8] test_timer_expires passed" << std::endl;

    test_timer_not_expired();
    std::cout << "[2/8] test_timer_not_expired passed" << std::endl;

    test_multiple_timers();
    std::cout << "[3/8] test_multiple_timers passed" << std::endl;

    test_recurring_timer();
    std::cout << "[4/8] test_recurring_timer passed" << std::endl;

    test_get_next_timer();
    std::cout << "[5/8] test_get_next_timer passed" << std::endl;

    test_cancel();
    std::cout << "[6/8] test_cancel passed" << std::endl;

    test_refresh();
    std::cout << "[7/8] test_refresh passed" << std::endl;

    test_reset();
    std::cout << "[8/8] test_reset passed" << std::endl;

    std::cout << "test_timer: all passed" << std::endl;
    return 0;
}
