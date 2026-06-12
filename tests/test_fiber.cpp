#include <cassert>
#include <exception>
#include <iostream>
#include <memory>

#include "netlib/fiber.h"
#include "netlib/log.h"

// ============================================================
// Fiber 测试 — 无 Scheduler 模式，直接 resume/yield 验证状态机
// ============================================================

// 所有测试前确保主协程已创建（runInScheduler=false 依赖 mainFiber）
static void ensureMainFiber() {
    netlib::Fiber::GetThis();
}

// 1. 创建 Fiber → resume → cb 执行完毕 → state 变为 TERM
void test_create_and_resume() {
    ensureMainFiber();
    bool called = false;

    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        called = true;
    }, 0, false);

    assert(fiber->getSate() == netlib::Fiber::State::READY);
    fiber->resume();
    // cb 执行完，MainFunc 设 TERM 后 yield 回这里
    assert(called);
    assert(fiber->getSate() == netlib::Fiber::State::TERM);
}

// 2. Fiber 中途 yield → 回到调用者 → 再次 resume 继续执行
void test_yield_and_resume() {
    ensureMainFiber();
    int step = 0;

    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        step = 1;
        netlib::Fiber::GetThis()->yield();  // 挂起，step 保留 = 1
        step = 2;
    }, 0, false);

    fiber->resume();
    assert(step == 1);
    assert(fiber->getSate() == netlib::Fiber::State::READY);  // yield 后变 READY

    fiber->resume();  // 从 yield 点继续
    assert(step == 2);
    assert(fiber->getSate() == netlib::Fiber::State::TERM);
}

// 3. 完整状态链：READY → RUNNING → (yield) → RUNNING → TERM
void test_state_chain() {
    ensureMainFiber();
    std::string log;

    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        log += "R";  // Running
        netlib::Fiber::GetThis()->yield();
        log += "T";  // Terminating
    }, 0, false);

    assert(fiber->getSate() == netlib::Fiber::State::READY);
    fiber->resume();
    assert(fiber->getSate() == netlib::Fiber::State::READY);  // yield 后变 READY
    assert(log == "R");

    fiber->resume();
    assert(fiber->getSate() == netlib::Fiber::State::TERM);
    assert(log == "RT");
}

// 4. GetThis() 返回主协程，GetFiberId() 返回有效值
void test_main_fiber() {
    auto main = netlib::Fiber::GetThis();
    assert(main != nullptr);
    assert(main->getSate() == netlib::Fiber::State::RUNNING);
    assert(main->getId() >= 0);

    uint64_t id = netlib::Fiber::GetFiberId();
    assert(id == main->getId());
}

// 5. cb 抛异常 → state 变为 EXCEPT
void test_exception() {
    ensureMainFiber();
    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        throw std::runtime_error("test exception");
    }, 0, false);

    fiber->resume();  // 异常被 MainFunc 捕获，设 EXCEPT 后 yield
    assert(fiber->getSate() == netlib::Fiber::State::EXCEPT);
}

// 6. reset() 复用已终止的协程
void test_reset() {
    ensureMainFiber();
    int value = 0;

    auto fiber = std::make_shared<netlib::Fiber>([&]() {
        value = 42;
    }, 0, false);

    fiber->resume();
    assert(fiber->getSate() == netlib::Fiber::State::TERM);
    assert(value == 42);

    // 复用同一个 fiber
    fiber->reset([&]() {
        value = 99;
    });
    assert(fiber->getSate() == netlib::Fiber::State::READY);

    fiber->resume();
    assert(fiber->getSate() == netlib::Fiber::State::TERM);
    assert(value == 99);
}

// 7. 多个 Fiber 交错执行，各自独立
void test_multiple_fibers() {
    ensureMainFiber();
    std::string trace;

    auto fa = std::make_shared<netlib::Fiber>([&]() {
        trace += "A1";
        netlib::Fiber::GetThis()->yield();
        trace += "A2";
    }, 0, false);

    auto fb = std::make_shared<netlib::Fiber>([&]() {
        trace += "B1";
        netlib::Fiber::GetThis()->yield();
        trace += "B2";
    }, 0, false);

    fa->resume();
    assert(trace == "A1");
    fb->resume();
    assert(trace == "A1B1");

    fa->resume();
    assert(trace == "A1B1A2");
    fb->resume();
    assert(trace == "A1B1A2B2");

    assert(fa->getSate() == netlib::Fiber::State::TERM);
    assert(fb->getSate() == netlib::Fiber::State::TERM);
}

int main() {
    test_create_and_resume();
    std::cout << "[1/7] test_create_and_resume passed" << std::endl;

    test_yield_and_resume();
    std::cout << "[2/7] test_yield_and_resume passed" << std::endl;

    test_state_chain();
    std::cout << "[3/7] test_state_chain passed" << std::endl;

    test_main_fiber();
    std::cout << "[4/7] test_main_fiber passed" << std::endl;

    test_exception();
    std::cout << "[5/7] test_exception passed" << std::endl;

    test_reset();
    std::cout << "[6/7] test_reset passed" << std::endl;

    test_multiple_fibers();
    std::cout << "[7/7] test_multiple_fibers passed" << std::endl;

    std::cout << "test_fiber: all passed" << std::endl;
    return 0;
}
