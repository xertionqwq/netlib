#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "netlib/log.h"
#include "netlib/thread.h"

// ============================================================
// Thread 测试 — 覆盖公开接口的边界
// ============================================================

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_thread failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

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

// 空函数对象和 nullptr 都应在调用线程中被拒绝，且不计入创建数。
void test_empty_callback() {
    const int before = netlib::Thread::numCreated();
    auto expectRejected = [](netlib::Thread::FunCallback callback) {
        bool rejected = false;
        try {
            netlib::Thread thread(callback);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "empty callback was not rejected by the constructor");
    };
    expectRejected(netlib::Thread::FunCallback{});
    expectRejected(nullptr);
    check(netlib::Thread::numCreated() == before,
          "rejected empty callbacks changed the thread creation count");
}

// 默认名称需要同步到工作线程 TLS；每个 Thread 只增加一次累计计数。
void test_default_name_and_count() {
    const int before = netlib::Thread::numCreated();
    std::string workerName;
    netlib::Thread unnamed([&] { workerName = netlib::Thread::GetName(); });
    check(unnamed.getId() > 0, "thread id was not published before construction returned");
    const std::string generatedName = unnamed.getName();
    unnamed.join();
    check(!generatedName.empty() && workerName == generatedName,
          "default name was not synchronized with worker TLS");
    check(netlib::Thread::numCreated() == before + 1,
          "default-named thread was counted more than once");

    netlib::Thread named([] {}, "explicit-name");
    named.join();
    check(named.getName() == "explicit-name", "explicit name was replaced");
    check(netlib::Thread::numCreated() == before + 2,
          "explicitly named thread was not counted exactly once");
}

// 工作线程错误地 join 自己时应报错；失败不能阻止外部析构继续回收线程。
void test_failed_self_join_cleanup() {
    std::promise<void> allowJoin;
    auto allowed = allowJoin.get_future();
    std::promise<bool> result;
    auto resultFuture = result.get_future();
    {
        netlib::Thread thread([&] {
            // 等待构造函数返回，保证底层 std::thread 已完成赋值。
            allowed.wait();
            bool rejected = false;
            try {
                netlib::Thread::GetThis()->join();
            } catch (const std::system_error& error) {
                rejected = error.code() ==
                    std::make_error_code(std::errc::resource_deadlock_would_occur);
            }
            result.set_value(rejected);
        }, "self-join");
        allowJoin.set_value();
        check(resultFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
              "self join blocked the worker");
        check(resultFuture.get(), "self join did not report a deadlock");
        // 由析构函数 join；此前一次失败的 join 不应使清理被跳过。
    }
}

int main() {
    test_create_and_join();
    std::cout << "[1/9] test_create_and_join passed" << std::endl;

    test_get_name();
    std::cout << "[2/9] test_get_name passed" << std::endl;

    test_get_this();
    std::cout << "[3/9] test_get_this passed" << std::endl;

    test_static_get_name();
    std::cout << "[4/9] test_static_get_name passed" << std::endl;

    test_set_name();
    std::cout << "[5/9] test_set_name passed" << std::endl;

    test_get_thread_id();
    std::cout << "[6/9] test_get_thread_id passed" << std::endl;

    test_empty_callback();
    std::cout << "[7/9] test_empty_callback passed" << std::endl;

    test_default_name_and_count();
    std::cout << "[8/9] test_default_name_and_count passed" << std::endl;

    test_failed_self_join_cleanup();
    std::cout << "[9/9] test_failed_self_join_cleanup passed" << std::endl;

    std::cout << "test_thread: all passed" << std::endl;
    return 0;
}
