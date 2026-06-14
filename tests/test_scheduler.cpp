#include <cassert>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "netlib/log.h"
#include "netlib/scheduler.h"

// ============================================================
// Scheduler 测试 — N 线程池 + M 协程共享就绪队列
// ============================================================

// 1. 单个 Fiber 正常执行
void test_single_fiber() {
    netlib::Scheduler sched(1, false, "test_single_fiber");
    std::atomic<int> value{0};

    sched.start();
    sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() {
        value = 42;
    }));
    sched.stop();

    assert(value == 42);
}

// 2. 多个 Fiber 全部执行
void test_multiple_fibers() {
    netlib::Scheduler sched(1, false, "test_multiple_fibers");
    std::atomic<int> counter{0};
    const int N = 10;

    sched.start();
    for (int i = 0; i < N; ++i) {
        sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() {
            counter++;
        }));
    }
    sched.stop();

    assert(counter == N);
}

// 3. Fiber 主动 yield 后重新被调度，最终执行完
void test_yield_and_resume() {
    // useCaller=true: 主线程自己参与调度，无需 join worker
    netlib::Scheduler sched(1, true, "test_yield_and_resume");
    std::atomic<int> step{0};

    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        step = 1;
        netlib::Fiber::GetThis()->yield();  // 返回调度器
        step = 2;
    });

    sched.start();
    sched.scheduleLock(fiber);
    sched.stop();

    assert(step == 2);
}

// 4. 多线程并行执行
void test_multi_thread() {
    const int N = 100;
    netlib::Scheduler sched(4, false, "test_multi_thread");
    std::atomic<int> counter{0};

    sched.start();
    for (int i = 0; i < N; ++i) {
        sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() {
            counter++;
        }));
    }
    sched.stop();

    assert(counter == N);
}

// 5. schedule 裸回调（非 Fiber）— 自动包装成 Fiber 执行
void test_schedule_callback() {
    netlib::Scheduler sched(1, false, "test_schedule_callback");
    std::atomic<int> value{0};

    sched.start();
    sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() {
        value = 99;
    }));
    sched.stop();

    assert(value == 99);
}

// 6. useCaller 模式 — 主线程参与调度
void test_use_caller() {
    // threads=1, useCaller=true → threadCount_=0，主线程就是惟一 worker
    netlib::Scheduler sched(1, true, "test_use_caller");
    std::atomic<int> value{0};

    sched.start();
    sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() {
        value = 1;
    }));
    sched.stop();  // 主线程在 stop() 里执行 schedulerFiber_->resume() → run()

    assert(value == 1);
}

// 7. 重复启停生命周期
void test_start_stop_cycle() {
    netlib::Scheduler sched(1, false, "test_start_stop_cycle");
    std::atomic<int> a{0}, b{0};

    // 第一轮
    sched.start();
    sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() { a = 1; }));
    sched.stop();
    assert(a == 1);

    // 第二轮 — 重新 start
    sched.start();
    sched.scheduleLock(netlib::Scheduler::FunCallBack([&]() { b = 2; }));
    sched.stop();
    assert(b == 2);
}

int main() {
    test_single_fiber();
    std::cout << "[1/7] test_single_fiber passed" << std::endl;

    test_multiple_fibers();
    std::cout << "[2/7] test_multiple_fibers passed" << std::endl;

    test_yield_and_resume();
    std::cout << "[3/7] test_yield_and_resume passed" << std::endl;

    test_multi_thread();
    std::cout << "[4/7] test_multi_thread passed" << std::endl;

    test_schedule_callback();
    std::cout << "[5/7] test_schedule_callback passed" << std::endl;

    test_use_caller();
    std::cout << "[6/7] test_use_caller passed" << std::endl;

    test_start_stop_cycle();
    std::cout << "[7/7] test_start_stop_cycle passed" << std::endl;

    std::cout << "test_scheduler: all passed" << std::endl;
    return 0;
}
