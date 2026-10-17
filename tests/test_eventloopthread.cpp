#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "netlib/eventloopthread.h"

using namespace netlib;
using namespace std::chrono_literals;

EventLoop* startLoopInOtherTranslationUnit(EventLoopThread& thread);
EventLoop* currentLoopInOtherTranslationUnit();

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_eventloopthread failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static void test_destroy_without_start() {
    const int before = Thread::numCreated();
    bool initialized = false;
    {
        EventLoopThread thread([&](EventLoop*) { initialized = true; });
    }
    check(!initialized, "construction invoked the initialization callback");
    check(Thread::numCreated() == before, "construction started a worker thread");
}

static void test_initialization_and_dispatch() {
    std::promise<std::pair<pid_t, std::string>> initialized;
    auto initializedFuture = initialized.get_future();
    std::promise<pid_t> dispatched;
    auto dispatchedFuture = dispatched.get_future();
    const pid_t callerTid = CurrentThread::tid();
    EventLoopThread thread([&](EventLoop* loop) {
        check(loop->isInLoopTread(), "EventLoop was constructed in the wrong thread");
        loop->setPollTimeoutMs(-1);
        initialized.set_value({CurrentThread::tid(), Thread::GetName()});
    }, "io-worker");

    EventLoop* loop = startLoopInOtherTranslationUnit(thread);
    check(loop != nullptr, "startLoop returned a null pointer");
    check(initializedFuture.wait_for(0s) == std::future_status::ready,
          "startLoop returned before initialization");
    const auto info = initializedFuture.get();
    check(info.first != callerTid && info.second == "io-worker",
          "initialization did not run in the named worker thread");

    loop->runInLoop([&] {
        check(currentLoopInOtherTranslationUnit() == loop,
              "EventLoop TLS differs across translation units");
        dispatched.set_value(CurrentThread::tid());
    });
    check(dispatchedFuture.wait_for(3s) == std::future_status::ready,
          "cross-thread task did not wake the EventLoop");
    check(dispatchedFuture.get() == info.first,
          "task did not run in the EventLoop thread");
}

static void test_repeated_start() {
    EventLoopThread thread;
    thread.startLoop();
    bool rejected = false;
    try {
        thread.startLoop();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    check(rejected, "a second startLoop call was accepted");
}

static void test_immediate_destruction() {
    // 同时覆盖尚未进入 loop() 就 quit，以及唤醒无限等待的 epoll_wait。
    for (int i = 0; i < 32; ++i) {
        EventLoopThread thread([](EventLoop* loop) { loop->setPollTimeoutMs(-1); });
        thread.startLoop();
    }
}

static void test_worker_exits_before_destruction() {
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    EventLoopThread thread;
    EventLoop* loop = thread.startLoop();
    loop->queueInLoop([&] {
        loop->quit();
        stopped.set_value_at_thread_exit();
    });
    check(stoppedFuture.wait_for(3s) == std::future_status::ready,
          "worker did not finish after quitting its loop");
    // loop 已销毁；包装器仍须 join 已结束的工作线程。
}

static void test_exit_during_startup() {
    for (int i = 0; i < 32; ++i) {
        EventLoopThread thread([](EventLoop* loop) { loop->quit(); });
        try {
            // 可能在循环退出之前拿到借用指针，此处不再使用它。
            check(thread.startLoop() != nullptr, "startup returned a null pointer");
        } catch (const std::runtime_error&) {
            // 若循环已退出，应报告失败，而不是永久等待就绪。
        }
    }
}

int main() {
    Logger::setLevel(LogLevel::WARN);
    test_destroy_without_start();
    test_initialization_and_dispatch();
    test_repeated_start();
    test_immediate_destruction();
    test_worker_exits_before_destruction();
    test_exit_during_startup();
    std::cout << "test_eventloopthread: all passed\n";
}
