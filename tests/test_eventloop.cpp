#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "netlib/eventloop.h"

using namespace netlib;
using namespace std::chrono_literals;

void constructEventLoopInOtherTranslationUnit();
pid_t threadIdInOtherTranslationUnit();

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_eventloop failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static int openFdCount() {
    DIR* directory = ::opendir("/proc/self/fd");
    check(directory != nullptr, "open fd directory");
    int count = 0;
    while (::readdir(directory))
        ++count;
    ::closedir(directory);
    return count;
}

static void test_dispatch_and_nested_queue() {
    EventLoop loop(-1);
    int calls = 0;
    loop.runInLoop([&] { ++calls; });
    check(calls == 1, "runInLoop did not run immediately in its owner thread");
    loop.queueInLoop([&] {
        ++calls;
        loop.queueInLoop([&] {
            ++calls;
            loop.quit();
        });
    });
    loop.loop();
    check(calls == 3, "pre-loop or nested tasks were not executed");
}

static void test_cross_thread_dispatch_and_quit() {
    std::promise<EventLoop*> ready;
    std::promise<void> completed, stopped, release;
    auto completedFuture = completed.get_future();
    auto stoppedFuture = stopped.get_future();
    auto releaseFuture = release.get_future();
    std::atomic_int calls{0};
    std::atomic_int completionPoll{0};
    const pid_t mainTid = CurrentThread::tid();
    std::thread worker([&] {
        EventLoop loop(-1);
        check(CurrentThread::tid() != mainTid, "thread ID cache was shared across threads");
        check(CurrentThread::tid() == ::syscall(SYS_gettid), "incorrect cached thread ID");
        ready.set_value(&loop);
        loop.loop();
        stopped.set_value();
        // 保持实例存活，直到外部 quit() 调用已经返回。
        releaseFuture.wait();
    });
    EventLoop* loop = ready.get_future().get();
    check(!loop->isInLoopTread(), "owner thread check accepted an external thread");
    loop->runInLoop([&] {
        check(loop->isInLoopTread(), "runInLoop executed in the submitting thread");
        ++calls;
    });

    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < 100; ++j)
                loop->queueInLoop([&] { ++calls; });
        });
    }
    for (auto& producer : producers)
        producer.join();
    loop->queueInLoop([&] {
        completionPoll = test::pollCalls.load();
        completed.set_value();
    });
    check(completedFuture.wait_for(3s) == std::future_status::ready,
          "cross-thread tasks did not wake the loop");
    check(calls == 401, "concurrent submissions were lost or duplicated");
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (test::pollCalls.load() <= completionPoll.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(test::pollCalls.load() > completionPoll.load(),
          "loop did not enter its next wait after completing tasks");
    loop->quit();
    check(stoppedFuture.wait_for(3s) == std::future_status::ready,
          "external quit did not wake an indefinitely blocked loop");
    release.set_value();
    worker.join();
}

static void test_quit_before_loop() {
    EventLoop loop(-1);
    const int before = test::pollCalls.load();
    loop.quit();
    loop.loop();
    check(test::pollCalls == before, "loop discarded an earlier quit request");
}

static void test_pending_exception_cleanup() {
    EventLoop loop(-1);
    loop.queueInLoop([] { throw std::runtime_error("task failure"); });
    bool caught = false;
    try {
        loop.loop();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught, "task exception was not propagated");
    loop.queueInLoop([&] { loop.quit(); });
    loop.loop();
}

static void test_io_exception_cleanup() {
    EventLoop loop(-1);
    int descriptors[2];
    check(::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2 failed");
    {
        Channel channel(&loop, descriptors[0]);
        channel.setReadCallback([&](Timestamp) {
            channel.disableAll();
            channel.remove();
            throw std::runtime_error("read callback failure");
        });
        channel.enableReading();
        check(::write(descriptors[1], "x", 1) == 1, "pipe write failed");
        bool caught = false;
        try {
            loop.loop();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "read callback exception was not propagated");
    }
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    loop.queueInLoop([&] { loop.quit(); });
    loop.loop();
}

static void test_poll_exception_cleanup() {
    EventLoop loop(-1);
    test::failNextPoll = true;
    bool caught = false;
    try {
        loop.loop();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught, "poll exception was not propagated");
    loop.queueInLoop([&] { loop.quit(); });
    loop.loop();
}

static void test_constructor_failure_cleanup() {
    const int before = openFdCount();
    test::failNextRegistration = true;
    bool caught = false;
    try {
        EventLoop loop;
    } catch (const std::system_error&) {
        caught = true;
    }
    check(caught, "injected registration failure was not reported");
    check(loopPtr == nullptr, "failed constructor retained its thread registration");
    check(openFdCount() == before, "failed constructor leaked its eventfd");
    constructEventLoopInOtherTranslationUnit();
    check(loopPtr == nullptr, "destructor retained its thread registration");
    check(openFdCount() == before, "successful construction/destruction leaked an fd");
    check(threadIdInOtherTranslationUnit() == CurrentThread::tid(),
          "thread-local ID was inconsistent across translation units");

    caught = false;
    try {
        EventLoop loop(-2);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    check(caught && loopPtr == nullptr && openFdCount() == before,
          "invalid constructor timeout was not cleaned up");
    EventLoop loop;
    caught = false;
    try {
        loop.setPollTimeoutMs(-2);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    check(caught, "invalid timeout setter value was accepted");
}

int main() {
    Logger::setLevel(LogLevel::ERROR);
    test_dispatch_and_nested_queue();
    test_cross_thread_dispatch_and_quit();
    test_quit_before_loop();
    test_pending_exception_cleanup();
    test_io_exception_cleanup();
    test_poll_exception_cleanup();
    test_constructor_failure_cleanup();
    std::cout << "test_eventloop: all passed\n";
}
