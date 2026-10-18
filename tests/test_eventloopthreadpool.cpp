#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netlib/eventloopthreadpool.h"

using namespace netlib;
using namespace std::chrono_literals;

void startPoolInOtherTranslationUnit(EventLoopThreadPool& pool);
EventLoop* nextPoolLoopInOtherTranslationUnit(EventLoopThreadPool& pool);
EventLoop* currentPoolLoopInOtherTranslationUnit();

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_eventloopthreadpool failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Callback>
static void expectException(Callback callback, const char* message) {
    try {
        callback();
    } catch (const Exception&) {
        return;
    }
    check(false, message);
}

template <typename Callback>
static void expectAbort(Callback callback) {
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const rlimit limit{0, 0};
        if (::setrlimit(RLIMIT_CORE, &limit) != 0)
            ::_exit(2);
        callback();
        ::_exit(0);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child, "waitpid failed");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "invalid thread or state did not abort");
}

static void test_construction_and_validation() {
    expectException<std::invalid_argument>([] {
        EventLoopThreadPool pool(nullptr, "invalid");
    }, "null baseLoop was accepted");

    EventLoop loop;
    const int before = Thread::numCreated();
    {
        EventLoopThreadPool pool(&loop, "unstarted");
        check(!pool.started() && pool.name() == "unstarted",
              "initial state or name is incorrect");
        expectException<std::invalid_argument>([&] { pool.setThreadNum(-1); },
                                               "negative thread count was accepted");
        pool.setThreadNum(2);
    }
    check(Thread::numCreated() == before, "construction or configuration started workers");
}

static void test_zero_workers_and_start_guards() {
    EventLoop loop;
    EventLoopThreadPool pool(&loop, "base-only");
    const int before = Thread::numCreated();
    int initialized = 0;
    pool.start([&](EventLoop* initializedLoop) {
        check(initializedLoop == &loop && loop.isInLoopTread(),
              "zero-worker initialization ran outside baseLoop");
        ++initialized;
        expectException<std::logic_error>([&] { pool.start(); },
                                          "reentrant startup was accepted");
        expectException<std::logic_error>([&] { pool.setThreadNum(1); },
                                          "configuration during startup was accepted");
    });
    check(pool.started() && initialized == 1, "zero-worker initialization did not finish");
    const auto loops = pool.getAllLoops();
    check(loops.size() == 1 && loops.front() == &loop, "zero-worker loop list is incorrect");
    for (int i = 0; i < 4; ++i)
        check(nextPoolLoopInOtherTranslationUnit(pool) == &loop,
              "zero-worker selection did not return baseLoop");
    expectException<std::logic_error>([&] { pool.start(); }, "repeated startup was accepted");
    expectException<std::logic_error>([&] { pool.setThreadNum(1); },
                                      "configuration after startup was accepted");
    check(initialized == 1 && Thread::numCreated() == before,
          "rejected startup changed initialization or worker count");

    EventLoopThreadPool withoutCallback(&loop, "empty-callback");
    startPoolInOtherTranslationUnit(withoutCallback);
    check(withoutCallback.started() && withoutCallback.getNextLoop() == &loop,
          "default empty initialization callback failed");
}

static void test_workers_dispatch_and_round_robin() {
    struct WorkerInfo {
        EventLoop* loop = nullptr;
        pid_t tid = 0;
        std::string name;
    };
    EventLoop baseLoop;
    const pid_t callerTid = CurrentThread::tid();
    std::vector<WorkerInfo> workers(3);
    std::vector<std::promise<void>> stopped(3);
    std::vector<std::future<void>> stoppedFutures;
    for (auto& promise : stopped)
        stoppedFutures.push_back(promise.get_future());
    std::atomic_int initialized{0};
    {
        EventLoopThreadPool pool(&baseLoop, "worker");
        pool.setThreadNum(3);
        pool.start([&](EventLoop* loop) {
            const int index = initialized.fetch_add(1);
            check(index < 3 && loop->isInLoopTread(), "invalid worker initialization");
            loop->setPollTimeoutMs(-1);
            workers[index] = {loop, CurrentThread::tid(), Thread::GetName()};
            stopped[index].set_value_at_thread_exit();
        });
        check(pool.started() && initialized == 3, "startup returned before initialization");
        const auto loops = pool.getAllLoops();
        check(loops.size() == 3, "incorrect worker loop count");
        check(loops[0] != loops[1] && loops[0] != loops[2] && loops[1] != loops[2],
              "workers share an EventLoop");
        for (EventLoop* loop : loops) {
            const auto info = std::find_if(workers.begin(), workers.end(),
                                          [loop](const WorkerInfo& worker) {
                                              return worker.loop == loop;
                                          });
            check(loop != &baseLoop && info != workers.end() && info->tid != callerTid,
                  "worker loop was not initialized in its own thread");
            std::promise<pid_t> dispatched;
            auto dispatchedFuture = dispatched.get_future();
            loop->queueInLoop([loop, &dispatched] {
                check(currentPoolLoopInOtherTranslationUnit() == loop,
                      "worker EventLoop TLS differs across translation units");
                dispatched.set_value(CurrentThread::tid());
            });
            check(dispatchedFuture.wait_for(3s) == std::future_status::ready,
                  "worker did not execute queued task");
            check(dispatchedFuture.get() == info->tid, "task ran in the wrong worker");
        }
        for (int i = 0; i < 9; ++i)
            check(nextPoolLoopInOtherTranslationUnit(pool) == loops[i % loops.size()],
                  "round-robin selection is incorrect");
        for (int i = 0; i < 3; ++i)
            check(workers[i].name == "worker" + std::to_string(i), "incorrect worker name");
    }
    for (auto& future : stoppedFutures)
        check(future.wait_for(0s) == std::future_status::ready,
              "pool destruction returned before workers exited");
    bool baseDispatched = false;
    baseLoop.queueInLoop([&] { baseDispatched = true; baseLoop.quit(); });
    baseLoop.loop();
    check(baseDispatched, "pool destruction stopped baseLoop");
}

static void test_immediate_destruction() {
    EventLoop loop;
    for (int i = 0; i < 8; ++i) {
        EventLoopThreadPool pool(&loop, "short-lived");
        pool.setThreadNum(2);
        startPoolInOtherTranslationUnit(pool);
    }
}

struct CopyFailureState {
    int copies = 0;
    std::atomic_int initialized{0};
    std::promise<void> stopped;
};

// 在第二个工作线程创建前，让初始化回调的复制抛异常，验证已启动线程的回收。
struct FailingCopyInit {
    explicit FailingCopyInit(std::shared_ptr<CopyFailureState> state) : state(std::move(state)) {}
    FailingCopyInit(FailingCopyInit&&) noexcept = default;
    FailingCopyInit(const FailingCopyInit& other) : state(other.state) {
        if (++state->copies == 2)
            throw std::runtime_error("injected callback copy failure");
    }
    void operator()(EventLoop* loop) const {
        loop->setPollTimeoutMs(-1);
        ++state->initialized;
        state->stopped.set_value_at_thread_exit();
    }
    std::shared_ptr<CopyFailureState> state;
};

static void test_startup_failure_cleanup() {
    EventLoop loop;
    auto state = std::make_shared<CopyFailureState>();
    auto stopped = state->stopped.get_future();
    EventLoopThreadPool pool(&loop, "partial-start");
    pool.setThreadNum(2);
    EventLoopThreadPool::ThreadInitCallback callback{FailingCopyInit(state)};
    expectException<std::runtime_error>([&] { pool.start(std::move(callback)); },
                                       "callback copy failure was not propagated");
    check(state->initialized == 1, "failure did not occur after one worker started");
    check(stopped.wait_for(0s) == std::future_status::ready,
          "startup failure left an existing worker running");
    check(!pool.started(), "failed startup was reported as successful");
    expectException<std::logic_error>([&] { pool.start(); }, "failed pool allowed retry");
    expectException<std::logic_error>([&] { pool.setThreadNum(0); },
                                      "failed pool allowed configuration");

    EventLoopThreadPool zeroWorkers(&loop, "base-init-failure");
    expectException<std::runtime_error>([&] {
        zeroWorkers.start([](EventLoop*) { throw std::runtime_error("base init failure"); });
    }, "baseLoop initialization failure was not propagated");
    check(!zeroWorkers.started(), "failed baseLoop initialization was reported as successful");
    expectException<std::logic_error>([&] { zeroWorkers.start(); },
                                      "baseLoop initialization failure allowed retry");
    bool dispatched = false;
    loop.queueInLoop([&] { dispatched = true; loop.quit(); });
    loop.loop();
    check(dispatched, "startup failure made baseLoop unusable");
}

static void test_invalid_loop_queries() {
    for (bool afterFailure : {false, true}) {
        for (bool allLoops : {false, true}) {
            expectAbort([=] {
                EventLoop loop;
                EventLoopThreadPool pool(&loop, "invalid-query");
                if (afterFailure) {
                    expectException<std::runtime_error>([&] {
                        pool.start([](EventLoop*) { throw std::runtime_error("init failure"); });
                    }, "expected initialization failure");
                }
                if (allLoops)
                    pool.getAllLoops();
                else
                    pool.getNextLoop();
            });
        }
    }
}

static void test_thread_affinity() {
    expectAbort([] {
        EventLoop loop;
        std::thread worker([&] { EventLoopThreadPool pool(&loop, "wrong-constructor"); });
        worker.join();
    });
    expectAbort([] {
        EventLoop loop;
        EventLoopThreadPool pool(&loop, "wrong-setter");
        std::thread worker([&] { pool.setThreadNum(1); });
        worker.join();
    });
    expectAbort([] {
        EventLoop loop;
        auto pool = std::make_unique<EventLoopThreadPool>(&loop, "wrong-destructor");
        std::thread worker([pool = std::move(pool)]() mutable { pool.reset(); });
        worker.join();
    });
}

int main() {
    Logger::setLevel(LogLevel::WARN);
    test_construction_and_validation();
    test_zero_workers_and_start_guards();
    test_workers_dispatch_and_round_robin();
    test_immediate_destruction();
    test_startup_failure_cleanup();
    test_invalid_loop_queries();
    test_thread_affinity();
    std::cout << "test_eventloopthreadpool: all passed\n";
}
