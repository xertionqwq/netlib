#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

#include "netlib/log.h"
#include "netlib/thread.h"

// ============================================================
// Thread 测试 — 覆盖公开接口的边界
// ============================================================

// 1. 构造 → callback 执行 → join，验证基本生命周期
void test_create_and_join() {
    bool called = false;
    netlib::Thread t([&]() { called = true; }, "test1");
    assert(t.getId() != -1 && "id should be set after construction");
    t.join();
    assert(called && "callback should have run");
}

// 2. getName() 返回构造时传入的名字
void test_get_name() {
    netlib::Thread t([]() {}, "worker");
    assert(t.getName() == "worker");
    t.join();
}

// 3. 在线程内部通过 GetThis() 拿到正确的 Thread*
void test_get_this() {
    netlib::Thread *captured = nullptr;
    netlib::Thread t([&]() {
        captured = netlib::Thread::GetThis();
    }, "test3");

    // 这里的 captured 在 join 前可能还没被设
    // 但实际上 join 等待线程结束，所以是安全的
    t.join();
    assert(captured == &t && "GetThis() should return this Thread");
    assert(captured != nullptr);
}

// 4. 在线程内部通过 GetName() 拿到正确的名字
void test_static_get_name() {
    std::string capturedName;
    netlib::Thread t([&]() {
        capturedName = netlib::Thread::GetName();
    }, "static-get-name");
    t.join();
    assert(capturedName == "static-get-name");
}

// 5. SetName() 修改线程名，GetName() 能看到
void test_set_name() {
    netlib::Thread t([]() {}, "original");
    netlib::Thread::SetName("changed");
    // 注：这里在主线程调用 SetName，测的是主线程的 TLS
    assert(netlib::Thread::GetName() == "changed");
    t.join();
}

// 6. GetThreadId() 返回有效的 tid（> 0）
void test_get_thread_id() {
    pid_t tid = netlib::Thread::GetThreadId();
    assert(tid > 0 && "thread id should be positive");
}

int main() {
    test_create_and_join();
    std::cout << "[1/6] test_create_and_join passed" << std::endl;

    test_get_name();
    std::cout << "[2/6] test_get_name passed" << std::endl;

    test_get_this();
    std::cout << "[3/6] test_get_this passed" << std::endl;

    test_static_get_name();
    std::cout << "[4/6] test_static_get_name passed" << std::endl;

    test_set_name();
    std::cout << "[5/6] test_set_name passed" << std::endl;

    test_get_thread_id();
    std::cout << "[6/6] test_get_thread_id passed" << std::endl;

    std::cout << "test_thread: all passed" << std::endl;
    return 0;
}
