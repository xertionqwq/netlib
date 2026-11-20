#include "netlib/timerQueue.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

namespace {

int timerFd = -1;
int registrationsUntilFailure = 0;
int failedRegistrationFd = -1;
bool fixedWallTime = false;
bool injectReads = false;
int readAttempts = 0;
int fullReads = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "test_timerqueue failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

extern "C" int __real_timerfd_create(int, int);
extern "C" int __wrap_timerfd_create(int clock, int flags) {
    timerFd = __real_timerfd_create(clock, flags);
    return timerFd;
}

extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
extern "C" int __wrap_epoll_ctl(int epollFd, int op, int fd, epoll_event *event) {
    if (op == EPOLL_CTL_ADD && registrationsUntilFailure > 0 && --registrationsUntilFailure == 0) {
        failedRegistrationFd = fd;
        errno = EIO;
        return -1;
    }
    return __real_epoll_ctl(epollFd, op, fd, event);
}

extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t size) {
    if (!injectReads || fd != timerFd)
        return __real_read(fd, buffer, size);

    // 不消耗真实就绪通知，下一轮仍能读到内核计数。
    switch (++readAttempts) {
    case 1:
        errno = EAGAIN;
        return -1;
    case 2:
        errno = EINTR;
        return -1;
    case 3:
        errno = EIO;
        return -1;
    case 4:
        return 4;
    default:
        const auto result = __real_read(fd, buffer, size);
        if (result == static_cast<ssize_t>(sizeof(uint64_t)))
            ++fullReads;
        return result;
    }
}

extern "C" int __real_gettimeofday(timeval *, void *);
extern "C" int __wrap_gettimeofday(timeval *time, void *zone) {
    if (!fixedWallTime)
        return __real_gettimeofday(time, zone);
    time->tv_sec = 100;
    time->tv_usec = 250'000;
    return 0;
}

namespace {

void testRelativeDelayUnits() {
    fixedWallTime = true;
    const auto delay = netlib::howMuchTimeFromNow(netlib::Timestamp(102'000'000));
    const auto overdue = netlib::howMuchTimeFromNow(netlib::Timestamp(100'000'000));
    fixedWallTime = false;
    check(delay.tv_sec == 1 && delay.tv_nsec == 750'000'000,
          "1.75 seconds was not converted to seconds and nanoseconds");
    check(overdue.tv_sec == 0 && overdue.tv_nsec == 100'000,
          "overdue timer must be armed with a positive 100-microsecond delay");
}

void testCancelBeforeInsertion() {
    netlib::EventLoop loop;
    netlib::TimerQueue queue(&loop);
    netlib::TimerId canceled;
    int canceledCalls = 0;
    int keptCalls = 0;
    const auto when = netlib::Timestamp::now();
    std::thread worker([&] {
        canceled = queue.addTimer([&] { ++canceledCalls; }, when, 0.0);
        queue.addTimer([&] {
            ++keptCalls;
            loop.quit();
        }, when, 0.0);
    });
    worker.join();

    queue.cancel(canceled);
    loop.loop();
    check(canceledCalls == 0 && keptCalls == 1,
          "loop-thread cancel must remove only the matching pending timer");
}

void testDestroyWithPendingWork() {
    netlib::EventLoop loop;
    std::weak_ptr<int> callbackPayload;
    {
        netlib::TimerQueue queue(&loop);
        const auto when = netlib::Timestamp::now();
        const auto active = queue.addTimer([] { check(false, "destroyed timer ran"); }, when, 0.0);
        std::thread worker([&] {
            auto payload = std::make_shared<int>(0);
            callbackPayload = payload;
            queue.addTimer([payload] { check(false, "destroyed pending timer ran"); }, when, 0.0);
            queue.cancel(active);
        });
        worker.join();
        check(!callbackPayload.expired(), "pending timer payload was released too early");
    }

    check(callbackPayload.expired(), "destroying the queue must release pending timers");
    bool drained = false;
    loop.queueInLoop([&] {
        drained = true;
        loop.quit();
    });
    loop.loop();
    check(drained, "loop did not drain the queued add and cancel requests");
}

void testConstructorClosesFdOnRegistrationFailure() {
    netlib::EventLoop loop;
    registrationsUntilFailure = 1;
    bool caught = false;
    try {
        netlib::TimerQueue queue(&loop);
    } catch (const std::system_error &error) {
        caught = error.code().value() == EIO;
    }
    check(caught && timerFd >= 0, "registration failure did not propagate");
    const int result = ::fcntl(timerFd, F_GETFD);
    check(result == -1 && errno == EBADF, "constructor failure leaked timerfd");
}

void testLoopConstructorClosesFdsOnRegistrationFailure() {
    for (int registration : {1, 2}) {
        registrationsUntilFailure = registration;
        failedRegistrationFd = -1;
        bool caught = false;
        try {
            netlib::EventLoop loop;
        } catch (const std::system_error &error) {
            caught = error.code().value() == EIO;
        }
        check(caught && failedRegistrationFd >= 0, "loop registration failure did not propagate");
        check(netlib::loopPtr == nullptr, "failed loop constructor left its thread registered");
        check(::fcntl(timerFd, F_GETFD) == -1 && errno == EBADF,
              "failed loop constructor leaked its timerfd");
        check(::fcntl(failedRegistrationFd, F_GETFD) == -1 && errno == EBADF,
              "failed loop constructor leaked the fd it was registering");
    }
}

void testEventLoopTimerMethods() {
    netlib::EventLoop loop(-1);
    int atCalls = 0;
    int afterCalls = 0;
    int everyCalls = 0;
    const auto finish = [&] {
        if (atCalls == 1 && afterCalls == 1 && everyCalls == 2)
            loop.quit();
    };

    loop.runAt(netlib::Timestamp::now(), [&] {
        ++atCalls;
        finish();
    });
    netlib::TimerId repeating;
    repeating = loop.runEvery(0.002, [&] {
        if (++everyCalls == 2)
            loop.cancel(repeating);
        finish();
    });

    std::thread worker([&] {
        const auto canceled = loop.runAfter(0.0, [] { check(false, "canceled timer ran"); });
        loop.cancel(canceled);
        loop.runAfter(0.0, [&] {
            check(loop.isInLoopTread(), "timer callback ran outside the owner loop");
            ++afterCalls;
            finish();
        });
    });
    worker.join();

    loop.loop();
    check(atCalls == 1 && afterCalls == 1 && everyCalls == 2,
          "EventLoop timer methods did not dispatch the expected callbacks");
}

void testUnsuccessfulReadsDoNotDispatch() {
    netlib::EventLoop loop;
    netlib::TimerQueue queue(&loop);
    injectReads = true;
    queue.addTimer([&] {
        check(readAttempts == 5 && fullReads == 1,
              "callback ran before an entire timerfd counter was read");
        loop.quit();
    }, netlib::Timestamp::now(), 0.0);
    loop.loop();
    injectReads = false;
}

struct TimerCallbackError {
    int code;
};

void testCallbackExceptionRestoresBatch() {
    using netlib::addTime;
    netlib::EventLoop loop;
    netlib::TimerQueue queue(&loop);
    const auto due = addTime(netlib::Timestamp::now(), -1.0);
    std::vector<int> order;
    netlib::TimerId completedRepeat;
    netlib::TimerId deferredRepeat;
    netlib::TimerId canceled;
    std::weak_ptr<int> failingPayload;

    int repeatCalls = 0;
    completedRepeat = queue.addTimer([&] {
        if (++repeatCalls == 1) {
            order.push_back(1);
        } else {
            order.push_back(5);
            queue.cancel(completedRepeat);
            loop.quit();
        }
    }, due, 0.002);
    {
        auto payload = std::make_shared<int>(42);
        failingPayload = payload;
        queue.addTimer([&, payload] {
            order.push_back(2);
            queue.cancel(canceled);
            throw TimerCallbackError{*payload};
        }, addTime(due, 0.001), 0.002);
    }
    queue.addTimer([&] { order.push_back(3); }, addTime(due, 0.002), 0.0);
    deferredRepeat = queue.addTimer([&] {
        order.push_back(4);
        queue.cancel(deferredRepeat);
    }, addTime(due, 0.003), 60.0);
    canceled = queue.addTimer([] { check(false, "canceled timer ran after recovery"); },
                              addTime(due, 0.004), 0.0);

    bool caught = false;
    try {
        loop.loop();
    } catch (const TimerCallbackError &error) {
        caught = error.code == 42;
    }
    check(caught, "loop did not propagate the original timer exception");
    check(order == std::vector<int>{1, 2}, "timer callback exception did not interrupt the batch");
    check(failingPayload.expired(), "failed repeating timer was kept alive");

    loop.loop();
    check(order == std::vector<int>{1, 2, 3, 4, 5},
          "recovered loop did not run deferred timers at their original deadlines and resume the repeat");
}

void testCallbackExceptionRearmsFutureTimer() {
    netlib::EventLoop loop;
    netlib::TimerQueue queue(&loop);
    fixedWallTime = true;
    const auto now = netlib::Timestamp::now();
    queue.addTimer([] { throw TimerCallbackError{7}; }, now, 0.0);
    bool futureRan = false;
    queue.addTimer([&] {
        futureRan = true;
        loop.quit();
    }, netlib::addTime(now, 0.010), 0.0);

    bool caught = false;
    try {
        loop.loop();
    } catch (const TimerCallbackError &error) {
        caught = error.code == 7;
    }
    fixedWallTime = false;
    check(caught && !futureRan, "future timer should still be waiting at the time of the exception");

    // 恢复真实时间后，依赖异常恢复时设置的 timerfd 唤醒第二次 loop()。
    loop.loop();
    check(futureRan, "loop did not wake for the remaining timer after a callback exception");
}

} // namespace

int main() {
    netlib::Logger::setLevel(netlib::LogLevel::FATAL);
    testRelativeDelayUnits();
    testCancelBeforeInsertion();
    testDestroyWithPendingWork();
    testConstructorClosesFdOnRegistrationFailure();
    testLoopConstructorClosesFdsOnRegistrationFailure();
    testEventLoopTimerMethods();
    testUnsuccessfulReadsDoNotDispatch();
    testCallbackExceptionRestoresBatch();
    testCallbackExceptionRearmsFutureTimer();
}
