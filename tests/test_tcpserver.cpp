#include "netlib/tcpServer.h"
#include "netlib/tcpServer.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <new>
#include <poll.h>
#include <set>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>

using namespace netlib;
using namespace std::chrono_literals;

std::unique_ptr<TcpServer> makeServerInOtherTranslationUnit(EventLoop*, TcpServer::Option);
void startServerInOtherTranslationUnit(TcpServer&);

namespace {
const char* currentTest = "setup";
thread_local int failAllocationAfter = -1;
std::atomic_int listenerFd{-1}, acceptedFd{-1}, acceptFault{0}, addressFaultFd{-1}, addFaultFd{-1};
std::atomic_bool throwCallbackCopy{false};
std::atomic_int watchedCloseFd{-1}, watchedCloses{0};
EventLoop* acceptLoop = nullptr;
bool quitAfterAccept = false;
std::promise<void>* listenerClosed = nullptr;

void check(bool value, const char* message) {
    if (!value) {
        std::cout << "[FAIL] " << currentTest << ": " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

// 分配故障仅作用于当前线程的一次分配，不影响工作线程或客户端。
void* operator new(std::size_t size) {
    if (failAllocationAfter == 0) {
        failAllocationAfter = -1;
        throw std::bad_alloc();
    }
    if (failAllocationAfter > 0)
        --failAllocationAfter;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

extern "C" int __real_socket(int, int, int);
extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __real_getsockname(int, sockaddr*, socklen_t*);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event*);
extern "C" int __real_close(int);

extern "C" int __wrap_socket(int domain, int type, int protocol) {
    const int fd = __real_socket(domain, type, protocol);
    listenerFd.store(fd);
    return fd;
}
extern "C" int __wrap_accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    const int result = __real_accept4(fd, address, length, flags);
    if (result >= 0) {
        acceptedFd.store(result);
        const int fault = acceptFault.exchange(0);
        if (fault == 1)
            failAllocationAfter = 0;
        else if (fault == 2)
            addressFaultFd.store(result);
        else if (fault == 3)
            throwCallbackCopy.store(true);
        else if (fault == 4)
            addFaultFd.store(result);
        if (quitAfterAccept) {
            quitAfterAccept = false;
            acceptLoop->queueInLoop([] { acceptLoop->quit(); });
        }
    }
    return result;
}
extern "C" int __wrap_getsockname(int fd, sockaddr* address, socklen_t* length) {
    int expected = fd;
    if (fd >= 0 && addressFaultFd.compare_exchange_strong(expected, -1)) {
        errno = EIO;
        return -1;
    }
    return __real_getsockname(fd, address, length);
}
extern "C" int __wrap_epoll_ctl(int epfd, int operation, int fd, epoll_event* event) {
    int expected = fd;
    if (operation == EPOLL_CTL_ADD && addFaultFd.compare_exchange_strong(expected, -1)) {
        errno = EIO;
        return -1;
    }
    return __real_epoll_ctl(epfd, operation, fd, event);
}
extern "C" int __wrap_close(int fd) {
    if (fd >= 0 && fd == watchedCloseFd.load())
        ++watchedCloses;
    const int result = __real_close(fd);
    if (fd == listenerFd.load() && listenerClosed) {
        auto* signal = listenerClosed;
        listenerClosed = nullptr;
        signal->set_value();
    }
    return result;
}

namespace {
bool closed(int fd) { return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

void waitFor(int fd, short events) {
    pollfd entry{fd, events, 0};
    int ready;
    do { ready = ::poll(&entry, 1, 3000); } while (ready < 0 && errno == EINTR);
    check(ready == 1 && (entry.revents & (events | POLLHUP)), "client readiness timed out");
}

std::unique_ptr<Socket> connectClient(int listener) {
    const auto address = Socket::getLocalAddr(listener);
    auto client = Socket::adopt(__real_socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    check(::connect(client->fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0,
          "loopback connect failed");
    const int flags = ::fcntl(client->fd(), F_GETFL);
    check(flags >= 0 && ::fcntl(client->fd(), F_SETFL, flags | O_NONBLOCK) == 0,
          "could not make client nonblocking");
    return client;
}

void sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        waitFor(fd, POLLOUT);
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "client send failed");
        sent += static_cast<size_t>(n);
    }
}

std::string receive(int fd, size_t length) {
    std::string data(length, '\0');
    size_t received = 0;
    while (received < length) {
        waitFor(fd, POLLIN);
        const ssize_t n = ::read(fd, data.data() + received, length - received);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "client reached EOF before reply");
        received += static_cast<size_t>(n);
    }
    return data;
}

void expectEof(int fd) {
    char byte;
    for (;;) {
        waitFor(fd, POLLIN);
        const ssize_t n = ::read(fd, &byte, 1);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n == 0, "expected server EOF without extra data");
        return;
    }
}

void echoClient(int listener) {
    auto client = connectClient(listener);
    const std::string message("echo\0binary\n", 12);
    sendAll(client->fd(), message);
    check(receive(client->fd(), message.size()) == message, "server corrupted binary echo");
    check(::shutdown(client->fd(), SHUT_WR) == 0, "client half-close failed");
    expectEof(client->fd());
}

template<class Callback>
void expectLogicError(Callback callback) {
    bool rejected = false;
    try { callback(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "operation after startup was not rejected");
}

template<class Callback>
void expectAbort(Callback callback) {
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const rlimit limit{0, 0};
        if (::setrlimit(RLIMIT_CORE, &limit) != 0 ||
            std::freopen("/dev/null", "w", stderr) == nullptr)
            ::_exit(2);
        callback();
        ::_exit(0);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    check(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "wrong-thread operation did not trigger a persistent assertion");
}

void test_configuration_and_startup() {
    for (auto option : {TcpServer::Option::kNoReusePort, TcpServer::Option::kReusePort}) {
        EventLoop loop;
        auto server = makeServerInOtherTranslationUnit(&loop, option);
        const int listener = listenerFd.load();
        int reuse = -1;
        socklen_t length = sizeof reuse;
        check(::getsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &reuse, &length) == 0 &&
              reuse == (option == TcpServer::Option::kReusePort), "reuseport option mapping failed");
        bool invalid = false;
        try { server->setThreadNum(-1); } catch (const std::invalid_argument&) { invalid = true; }
        check(invalid, "negative worker count was accepted");
        int initialized = 0;
        server->setThreadInitCallback([&](EventLoop* ioLoop) {
            check(ioLoop == &loop && loop.isInLoopTread(), "zero-thread initialization owner changed");
            ++initialized;
            expectLogicError([&] { server->start(); }); // 初始化回调重入启动。
        });
        startServerInOtherTranslationUnit(*server);
        check(initialized == 1, "thread initialization callback was not forwarded");
        expectLogicError([&] { server->start(); });
        expectLogicError([&] { server->setThreadNum(1); });
        expectLogicError([&] { server->setMessageCallback({}); });
        server.reset();
        check(closed(listener), "server destruction retained listening fd");
    }
    for (bool failListen : {false, true}) {
        EventLoop loop;
        auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
        const int listener = listenerFd.load();
        if (failListen) {
            server->setThreadNum(1);
            addFaultFd.store(listener);
        } else {
            server->setThreadInitCallback([](EventLoop*) { throw std::runtime_error("init failed"); });
        }
        bool failed = false;
        try { server->start(); } catch (const std::exception&) { failed = true; }
        check(failed, "startup failure was not propagated");
        expectLogicError([&] { server->start(); });
        expectLogicError([&] { server->setThreadNum(0); });
        server.reset();
        check(closed(listener), "failed startup leaked listener");
    }
}

void test_echo_and_round_robin() {
    for (int workers : {0, 2}) {
        EventLoop loop;
        auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
        const int listener = listenerFd.load();
        const int port = InetAddress(Socket::getLocalAddr(listener)).port();
        server->setThreadNum(workers);
        std::mutex mutex;
        std::vector<EventLoop*> initialized, assigned;
        std::vector<std::string> names;
        std::vector<std::weak_ptr<TcpConnection>> weak;
        std::atomic_int down{0}, completed{0};
        server->setThreadInitCallback([&](EventLoop* ioLoop) {
            check(ioLoop->isInLoopTread(), "initialization ran outside its loop");
            std::lock_guard<std::mutex> lock(mutex);
            initialized.push_back(ioLoop);
        });
        server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
            check(conn->getLoop()->isInLoopTread(), "connection callback ran outside its loop");
            if (conn->connected()) {
                check(conn->localAddress().port() == port && conn->peerAddress().ip() == "127.0.0.1",
                      "connection addresses were not forwarded");
                std::lock_guard<std::mutex> lock(mutex);
                assigned.push_back(conn->getLoop());
                names.push_back(conn->name());
                weak.push_back(conn);
            } else {
                ++down;
            }
        });
        server->setMessageCallback([](const TcpConnectionPtr& conn, Buffer* b, Timestamp) { conn->send(b); });
        server->setWriteCompleteCallback([&](const TcpConnectionPtr& conn) {
            check(conn->getLoop()->isInLoopTread(), "write completion ran outside its loop");
            ++completed;
        });
        server->start();
        check(initialized.size() == static_cast<size_t>(workers == 0 ? 1 : workers),
              "worker initialization count changed");
        std::thread clients([&] {
            for (int i = 0; i < 6; ++i)
                echoClient(listener);
            loop.queueInLoop([&] { loop.quit(); });
        });
        loop.loop();
        clients.join();
        server.reset();
        check(assigned.size() == 6 && down == 6 && completed >= 6 &&
              std::set<std::string>(names.begin(), names.end()).size() == 6,
              "connection lifecycle, completion, or unique names changed");
        for (size_t i = 0; i < assigned.size(); ++i) {
            check(assigned[i] == assigned[i % (workers == 0 ? 1 : workers)], "worker selection was not round robin");
            check((assigned[i] == &loop) == (workers == 0), "zero-thread fallback or worker selection failed");
            check(weak[i].expired(), "server retained a closed connection");
        }
        if (workers == 2)
            check(assigned[0] != assigned[1], "both workers received the same loop");
    }
}

void test_optional_callbacks_and_discarded_messages() {
    EventLoop loop;
    auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
    const int listener = listenerFd.load();
    server->setMessageCallback({}); // 显式清空也应使用默认消费策略。
    server->start();
    std::thread client([&] {
        auto socket = connectClient(listener);
        sendAll(socket->fd(), std::string(256 * 1024, 'd'));
        check(::shutdown(socket->fd(), SHUT_WR) == 0, "client half-close failed");
        expectEof(socket->fd());
        loop.queueInLoop([&] { loop.quit(); });
    });
    loop.loop();
    client.join();
    server.reset();
    check(closed(listener), "default callbacks retained the listener");
}

void test_active_connections_on_destruction() {
    for (int workers : {0, 2}) {
        EventLoop loop;
        auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
        const int listener = listenerFd.load();
        server->setThreadNum(workers);
        std::mutex mutex;
        std::vector<std::weak_ptr<TcpConnection>> weak;
        std::atomic_int up{0}, down{0};
        std::promise<void> destroyed;
        auto destruction = destroyed.get_future();
        server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    weak.push_back(conn);
                }
                if (++up == 4)
                    loop.queueInLoop([&] { loop.quit(); });
            } else {
                ++down;
            }
        });
        server->start();
        std::thread clients([&] {
            std::vector<std::unique_ptr<Socket>> sockets;
            for (int i = 0; i < 4; ++i)
                sockets.push_back(connectClient(listener));
            check(destruction.wait_for(3s) == std::future_status::ready, "active destruction timed out");
            for (const auto& client : sockets)
                expectEof(client->fd());
        });
        loop.loop();
        server.reset();
        check(down == 4 && closed(listener), "destruction did not clean active connections or listener");
        for (const auto& conn : weak)
            check(conn.expired(), "destruction retained an active connection");
        destroyed.set_value();
        clients.join();
    }
}

void test_late_close_notification_after_server_destruction() {
    for (int workers : {0, 2}) {
        EventLoop loop;
        auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
        const int listener = listenerFd.load();
        server->setThreadNum(workers);
        std::weak_ptr<TcpConnection> weak;
        std::atomic_int down{0};
        server->setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                weak = conn;
                conn->forceClose();
            } else {
                ++down;
                // 该任务先于内部关闭通知入队，在事件分发之外销毁 Server。
                loop.queueInLoop([&] {
                    server.reset();
                    loop.queueInLoop([&] { loop.quit(); });
                });
            }
        });
        server->start();
        std::thread client([&] { auto socket = connectClient(listener); expectEof(socket->fd()); });
        loop.loop();
        client.join();
        check(!server && down == 1 && weak.expired(), "late close task retained or accessed dead Server");
    }
}

void test_destruction_waits_for_queued_establishment() {
    EventLoop loop;
    auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
    const int listener = listenerFd.load();
    server->setThreadNum(1);
    std::promise<void> entered, release, listenerGone;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    auto listenerFuture = listenerGone.get_future();
    std::atomic_int notifications{0};
    server->setThreadInitCallback([&](EventLoop* ioLoop) {
        ioLoop->queueInLoop([&] { entered.set_value(); releaseFuture.wait(); });
    });
    server->setConnectionCallback([&](const TcpConnectionPtr&) { ++notifications; });
    server->start();
    check(enteredFuture.wait_for(3s) == std::future_status::ready, "worker blocker did not start");
    auto client = connectClient(listener);
    acceptLoop = &loop;
    quitAfterAccept = true;
    loop.loop(); // conn 已入表，但所属工作 loop 尚未执行建立任务。
    listenerClosed = &listenerGone;
    std::thread unblock([&] {
        check(listenerFuture.wait_for(3s) == std::future_status::ready, "listener was not closed first");
        release.set_value();
    });
    server.reset();
    unblock.join();
    expectEof(client->fd());
    check(notifications == 0 && closed(listener), "cancelled establishment notified or leaked listener");
    acceptLoop = nullptr;
}

struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) {
        if (throwCallbackCopy.exchange(false))
            throw std::bad_alloc();
    }
    void operator()(const TcpConnectionPtr&) const {}
};

void test_rejected_connection_recovery() {
    for (int workers : {0, 2}) {
        for (int fault : {1, 2, 3, 4}) {
            EventLoop loop;
            auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
            const int listener = listenerFd.load();
            server->setThreadNum(workers);
            if (fault == 3)
                server->setConnectionCallback(ThrowingCopy{});
            server->setMessageCallback([](const TcpConnectionPtr& conn, Buffer* b, Timestamp) { conn->send(b); });
            server->start();
            acceptFault.store(fault);
            std::thread clients([&] {
                { auto rejected = connectClient(listener); expectEof(rejected->fd()); }
                echoClient(listener); // 单次失败不应停止整个接入循环。
                loop.queueInLoop([&] { loop.quit(); });
            });
            loop.loop();
            clients.join();
            server.reset();
            check(acceptFault == 0 && addressFaultFd == -1 && addFaultFd == -1 && !throwCallbackCopy,
                  "connection fault was not consumed");
        }
    }
}

void test_socket_and_constructor_ownership() {
    EventLoop loop;
    for (int failure : {0, 1, 2, 3, 4}) {
        int pair[2];
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair failed");
        Socket peer(pair[1]);
        watchedCloseFd.store(pair[0]);
        watchedCloses.store(0);
        bool failed = false;
        const InetAddress address(0);
        const std::string longName(256, 'n');
        try {
            if (failure == 0) {
                failAllocationAfter = 0;
                auto socket = Socket::adopt(pair[0]);
            } else if (failure == 1) {
                failAllocationAfter = 1; // Socket 创建成功，名称复制失败。
                TcpConnection conn(&loop, longName, pair[0], address, address);
            } else if (failure == 2) {
                failAllocationAfter = 1; // Socket 创建成功，Channel 分配失败。
                TcpConnection conn(&loop, "short", pair[0], address, address);
            } else if (failure == 3) {
                TcpConnection conn(nullptr, "short", pair[0], address, address);
            } else {
                auto socket = Socket::adopt(pair[0]);
                failAllocationAfter = 0; // make_shared 尚未进入连接构造函数。
                auto conn = std::make_shared<TcpConnection>(&loop, "short", std::move(socket), address, address);
            }
        } catch (const std::exception&) { failed = true; }
        check(failed && failAllocationAfter == -1 && closed(pair[0]) && watchedCloses == 1,
              "constructor failure leaked or closed fd more than once");
        watchedCloseFd.store(-1);
    }
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair failed");
    Socket peer(pair[1]);
    {
        TcpConnection cancelled(&loop, "cancelled", pair[0], InetAddress(0), InetAddress(0));
    }
    check(closed(pair[0]), "cancelling an unestablished connection leaked fd");
    bool failed = false;
    try { Socket::getLocalAddr(-1); } catch (const std::system_error& error) {
        failed = error.code().value() == EBADF && std::string(error.what()).find("fd=-1") != std::string::npos;
    }
    check(failed, "getLocalAddr did not propagate the original error and fd");
}

void test_thread_affinity() {
    for (int operation : {0, 1, 2}) {
        expectAbort([=] {
            EventLoop loop;
            auto server = makeServerInOtherTranslationUnit(&loop, TcpServer::Option::kNoReusePort);
            std::thread other([&] {
                if (operation == 0)
                    server->start();
                else if (operation == 1)
                    server->setMessageCallback({});
                else
                    server.reset();
            });
            other.join();
        });
    }
}
} // namespace

int main() {
    Logger::setLevel(LogLevel::ERROR);
    const struct { const char* name; void (*run)(); } tests[] = {
        {"configuration_and_startup", test_configuration_and_startup},
        {"echo_and_round_robin", test_echo_and_round_robin},
        {"optional_callbacks_and_discarded_messages", test_optional_callbacks_and_discarded_messages},
        {"active_connections_on_destruction", test_active_connections_on_destruction},
        {"late_close_notification", test_late_close_notification_after_server_destruction},
        {"queued_establishment_on_destruction", test_destruction_waits_for_queued_establishment},
        {"rejected_connection_recovery", test_rejected_connection_recovery},
        {"socket_and_constructor_ownership", test_socket_and_constructor_ownership},
        {"thread_affinity", test_thread_affinity},
    };
    for (const auto& test : tests) {
        currentTest = test.name;
        std::cout << "[RUN] " << currentTest << std::endl;
        test.run();
        std::cout << "[PASS] " << currentTest << '\n';
    }
    std::cout << "test_tcpserver: all passed\n";
}
