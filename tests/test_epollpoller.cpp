#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

#include "netlib/eventloop.h"

using namespace netlib;
using namespace std::chrono_literals;

static std::atomic_int nextWaitError{0};
static volatile std::sig_atomic_t signalReceived = 0;

// 仅注入一次等待错误，其余调用全部进入真实的 epoll_wait。
extern "C" int __real_epoll_wait(int, epoll_event*, int, int);
extern "C" int __wrap_epoll_wait(int epfd, epoll_event* events, int count, int timeout) {
    const int error = nextWaitError.exchange(0);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return __real_epoll_wait(epfd, events, count, timeout);
}

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_epollpoller failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class SocketPair : private Noncopyable {
public:
    SocketPair() {
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0, fds_) == 0, "socketpair failed");
    }
    ~SocketPair() {
        ::close(fds_[0]);
        ::close(fds_[1]);
    }
    int fd(int index) const { return fds_[index]; }
private:
    int fds_[2]{-1, -1};
};

static void test_timeout_and_timestamp() {
    EventLoop loop;
    EPollPoller poller(&loop);
    Poller::ChannelList active;
    const std::string before = Timestamp::now().toString();
    const Timestamp received = poller.poll(0, active);
    const std::string after = Timestamp::now().toString();
    check(active.empty(), "zero timeout returned stale events");
    check(received.toString() == before || received.toString() == after,
          "poll did not return its current batch timestamp");

    const auto start = std::chrono::steady_clock::now();
    poller.poll(20, active);
    check(std::chrono::steady_clock::now() - start >= 10ms,
          "finite timeout returned immediately");
    check(active.empty(), "timeout returned stale events");
}

static void test_event_collection_and_growth() {
    EventLoop loop(-1);
    constexpr int count = 80;
    std::vector<std::unique_ptr<SocketPair>> pairs;
    std::vector<std::unique_ptr<Channel>> channels;
    std::vector<int> hits(count, 0), batches;
    int received = 0;
    int previousBatch = 0;
    bool summaryQueued = false;

    for (int i = 0; i < count; ++i) {
        pairs.push_back(std::make_unique<SocketPair>());
        auto channel = std::make_unique<Channel>(&loop, pairs[i]->fd(0));
        channel->setReadCallback([&, i](Timestamp time) {
            char byte;
            check(::read(pairs[i]->fd(0), &byte, 1) == 1 && byte == i,
                  "incorrect Channel pointer or repeated stale event");
            check(++hits[i] == 1, "Channel was dispatched more than once");
            check(time.toString() == loop.pollReturnTime().toString(),
                  "batch timestamp was not forwarded to the callback");
            ++received;
            if (!summaryQueued) {
                summaryQueued = true;
                loop.queueInLoop([&] {
                    batches.push_back(received - previousBatch);
                    previousBatch = received;
                    summaryQueued = false;
                });
            }
            if (received == count)
                loop.quit();
        });
        channel->enableReading();
        channels.push_back(std::move(channel));
        const char byte = static_cast<char>(i);
        check(::write(pairs[i]->fd(1), &byte, 1) == 1, "socket write failed");
    }

    loop.loop();
    check(received == count && previousBatch == count, "ready Channels were lost");
    check(!batches.empty() && *std::max_element(batches.begin(), batches.end()) > 16,
          "full event buffer did not grow for later batches");
    for (auto& channel : channels) {
        channel->disableAll();
        channel->remove();
    }
}

static void markSignal(int) {
    signalReceived = 1;
}

static void test_signal_interruption() {
    EventLoop loop;
    EPollPoller poller(&loop);
    Poller::ChannelList active;
    struct sigaction action{}, previousAction{};
    action.sa_handler = markSignal;
    ::sigemptyset(&action.sa_mask);
    check(::sigaction(SIGUSR1, &action, &previousAction) == 0, "install signal handler");
    sigset_t signals, previousMask;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGUSR1);
    check(::pthread_sigmask(SIG_UNBLOCK, &signals, &previousMask) == 0,
          "unblock test signal");

    signalReceived = 0;
    const pthread_t target = ::pthread_self();
    std::atomic_bool finished{false};
    std::thread sender([&] {
        // 重复发送以覆盖信号先于 epoll_wait 到达的调度情况。
        while (!finished) {
            std::this_thread::sleep_for(5ms);
            if (!finished)
                check(::pthread_kill(target, SIGUSR1) == 0, "send test signal");
        }
    });
    bool threw = false;
    try {
        poller.poll(-1, active);
    } catch (...) {
        threw = true;
    }
    finished = true;
    sender.join();
    check(::sigaction(SIGUSR1, &previousAction, nullptr) == 0, "restore signal handler");
    check(::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr) == 0,
          "restore signal mask");
    check(signalReceived && !threw && active.empty(), "EINTR was not an empty batch");
}

static void test_wait_error_and_loop_recovery() {
    EventLoop loop(-1);
    nextWaitError = EBADF;
    bool caught = false;
    try {
        loop.loop();
    } catch (const std::system_error& error) {
        const std::string message = error.what();
        caught = error.code().value() == EBADF
            && message.find("epoll_wait epfd=") != std::string::npos
            && message.find("timeoutMs=-1") != std::string::npos;
    }
    check(caught && nextWaitError == 0, "wait failure lost its errno or context");
    bool ran = false;
    loop.queueInLoop([&] {
        ran = true;
        loop.quit();
    });
    loop.loop();
    check(ran, "EventLoop could not restart after a wait exception");
}

static void test_cross_thread_wakeup() {
    std::promise<EventLoop*> ready;
    std::promise<void> stopped, release;
    auto stoppedFuture = stopped.get_future();
    auto releaseFuture = release.get_future();
    std::thread worker([&] {
        EventLoop loop(-1);
        ready.set_value(&loop);
        loop.loop();
        stopped.set_value();
        releaseFuture.wait();
    });
    EventLoop* loop = ready.get_future().get();
    std::atomic_int calls{0};
    loop->queueInLoop([&] {
        check(loop->isInLoopTread(), "queued task ran in the wrong thread");
        ++calls;
    });
    loop->queueInLoop([&] { loop->quit(); });
    check(stoppedFuture.wait_for(3s) == std::future_status::ready,
          "eventfd did not wake the real epoll backend");
    check(calls == 1, "cross-thread task was lost or duplicated");
    release.set_value();
    worker.join();
}

int main() {
    Logger::setLevel(LogLevel::ERROR);
    test_timeout_and_timestamp();
    test_event_collection_and_growth();
    test_signal_interruption();
    test_wait_error_and_loop_recovery();
    test_cross_thread_wakeup();
    std::cout << "test_epollpoller: all passed\n";
}
