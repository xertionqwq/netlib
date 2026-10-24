#include "netlib/tcpConnection.h"
#include "netlib/tcpConnection.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <vector>

using namespace netlib;

TcpConnectionPtr makeTcpConnectionInOtherTranslationUnit(EventLoop* loop, int fd);
void shutdownTcpConnectionInOtherTranslationUnit(const TcpConnectionPtr& conn);
void forceCloseTcpConnectionInOtherTranslationUnit(const TcpConnectionPtr& conn);

namespace {
const char* currentTest = "setup";

void check(bool condition, const char* message) {
    if (!condition) {
        // 错误路径测试会捕获 stderr，失败诊断使用 stdout 避免被一起捕获。
        std::cout << "[FAIL] " << currentTest << ": " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

struct IoProbe {
    int fd = -1;
    std::thread::id owner;
    std::vector<int> readErrors, sendErrors;
    size_t nextReadError = 0, nextSendError = 0;
    size_t shortSend = 0;
    int reads = 0, shutdowns = 0;
    bool blockSends = false;
};
IoProbe io;

void checkIoThread() {
    check(std::this_thread::get_id() == io.owner, "socket I/O ran outside the owning loop thread");
}
} // namespace

// 只包装当前被测连接，其他 fd 及没有注入的调用使用真实 Linux 接口。
extern "C" ssize_t __real_readv(int, const iovec*, int);
extern "C" ssize_t __wrap_readv(int fd, const iovec* vectors, int count) {
    if (fd == io.fd) {
        checkIoThread();
        ++io.reads;
        if (io.nextReadError < io.readErrors.size()) {
            errno = io.readErrors[io.nextReadError++];
            return -1;
        }
    }
    return __real_readv(fd, vectors, count);
}

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t length, int flags) {
    if (fd == io.fd) {
        checkIoThread();
        check((flags & MSG_NOSIGNAL) != 0, "socket send omitted MSG_NOSIGNAL");
        if (io.nextSendError < io.sendErrors.size()) {
            errno = io.sendErrors[io.nextSendError++];
            return -1;
        }
        if (io.blockSends) {
            errno = EAGAIN;
            return -1;
        }
        if (io.shortSend != 0) {
            length = std::min(length, io.shortSend);
            io.shortSend = 0;
        }
    }
    return __real_send(fd, data, length, flags);
}

extern "C" int __real_shutdown(int, int);
extern "C" int __wrap_shutdown(int fd, int how) {
    if (fd == io.fd) {
        checkIoThread();
        if (how == SHUT_WR)
            ++io.shutdowns;
    }
    return __real_shutdown(fd, how);
}

namespace {
class SocketPair : private Noncopyable {
public:
    SocketPair() {
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0, fds_) == 0, "socketpair failed");
        const int size = 4096;
        check(::setsockopt(fds_[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0,
              "could not limit kernel send buffer");
    }
    ~SocketPair() {
        if (fds_[0] >= 0)
            ::close(fds_[0]);
        closePeer();
    }
    int releaseConnection() {
        const int result = fds_[0];
        fds_[0] = -1;
        return result;
    }
    int peer() const { return fds_[1]; }
    void closePeer() {
        if (fds_[1] >= 0)
            ::close(fds_[1]);
        fds_[1] = -1;
    }
private:
    int fds_[2]{-1, -1};
};

struct Fixture : private Noncopyable {
    EventLoop loop;
    SocketPair pair;
    TcpConnectionPtr conn;
    const int fd;

    Fixture() : fd(pair.releaseConnection()) {
        io = IoProbe{};
        io.fd = fd;
        io.owner = std::this_thread::get_id();
        conn = makeTcpConnectionInOtherTranslationUnit(&loop, fd);
    }
    ~Fixture() {
        if (conn) {
            // 用例中的局部变量可能已析构，兜底清理不再调用业务通知。
            conn->setConnectionCallback({});
            conn->connectDestroyed();
            conn.reset();
        }
        io.fd = -1;
    }
    void runPending() {
        loop.queueInLoop([this] { loop.quit(); });
        loop.loop();
    }
};

class LogCapture : private Noncopyable {
public:
    LogCapture() {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        file_ = std::tmpfile();
        check(saved_ >= 0 && file_ != nullptr, "log capture setup failed");
        check(::dup2(::fileno(file_), STDERR_FILENO) >= 0, "log redirection failed");
    }
    ~LogCapture() {
        restore();
        std::fclose(file_);
    }
    std::string finish() {
        restore();
        check(std::fseek(file_, 0, SEEK_SET) == 0, "log rewind failed");
        std::string result;
        char bytes[1024];
        size_t count;
        while ((count = std::fread(bytes, 1, sizeof bytes, file_)) != 0)
            result.append(bytes, count);
        check(!std::ferror(file_), "log read failed");
        return result;
    }
private:
    void restore() {
        if (saved_ >= 0) {
            std::fflush(stderr);
            check(::dup2(saved_, STDERR_FILENO) >= 0, "log restoration failed");
            ::close(saved_);
            saved_ = -1;
        }
    }
    int saved_ = -1;
    std::FILE* file_ = nullptr;
};

bool fdClosed(int fd) {
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void waitReadable(int fd) {
    pollfd entry{fd, POLLIN, 0};
    int ready;
    do {
        ready = ::poll(&entry, 1, 3000);
    } while (ready < 0 && errno == EINTR);
    check(ready == 1 && (entry.revents & (POLLIN | POLLHUP)), "peer receive timed out");
}

std::string receiveExactly(int fd, size_t length) {
    std::string result(length, '\0');
    size_t received = 0;
    while (received < length) {
        waitReadable(fd);
        const ssize_t n = ::read(fd, result.data() + received, length - received);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "peer closed before expected bytes arrived");
        received += static_cast<size_t>(n);
    }
    return result;
}

std::string receiveUntilEof(int fd, size_t maximum) {
    std::string result;
    char bytes[8192];
    for (;;) {
        waitReadable(fd);
        const ssize_t n = ::read(fd, bytes, sizeof bytes);
        if (n == 0)
            return result;
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "peer read failed");
        result.append(bytes, static_cast<size_t>(n));
        check(result.size() <= maximum, "peer received unexpected extra bytes");
    }
}

void sendPeer(int fd, const std::string& data) {
    ssize_t n;
    do {
        n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    check(n >= 0 && static_cast<size_t>(n) == data.size(), "small peer message could not be sent");
}

std::string pattern(size_t length) {
    std::string result(length, '\0');
    for (size_t i = 0; i < length; ++i)
        result[i] = static_cast<char>(i % 251);
    return result;
}

void test_creation_and_optional_callbacks() {
    for (bool establish : {false, true}) {
        Fixture f;
        check(f.conn->getLoop() == &f.loop && f.conn->name() == "tcpconnection-test",
              "connection identity changed");
        check(f.conn->localAddress().port() == 8080 &&
              f.conn->peerAddress().toIpPort() == "127.0.0.1:9000", "address metadata changed");
        check(!f.conn->connected() && !f.conn->disconnected(), "initial state is not Connecting");
        Buffer untouched;
        untouched.append("retained", 8);
        f.conn->send(&untouched);
        check(untouched.readableBytes() == 8, "unestablished send consumed a Buffer");
        if (establish)
            f.conn->connectEstablished(); // 不设置任何回调也应允许建立与清理。
        f.conn->connectDestroyed();
        f.conn->connectDestroyed();
        f.conn->send(&untouched);
        f.conn->shutdown();
        f.conn->forceClose();
        check(f.conn->disconnected() && untouched.readableBytes() == 8 && !fdClosed(f.fd),
              "cleanup state, rejected Buffer, or Socket ownership changed");
        f.conn.reset();
        check(fdClosed(f.fd), "last connection reference did not release fd");
    }
}

void test_read_binary_timestamp_and_eof() {
    Fixture f;
    std::string received, lifecycle;
    int closed = 0;
    f.conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
        lifecycle += c->connected() ? 'U' : 'D';
    });
    f.conn->setMessageCallback([&](const TcpConnectionPtr& c, Buffer* buffer, Timestamp time) {
        check(c == f.conn && f.loop.isInLoopTread(), "message callback owner changed");
        check(time.toString() == f.loop.pollReturnTime().toString(), "receive timestamp was lost");
        received += buffer->retrieveAllAsString();
    });
    f.conn->setCloseCallback([&](const TcpConnectionPtr& c) {
        check(c->disconnected(), "close callback ran before state changed");
        ++closed;
        lifecycle += 'C';
        f.loop.quit();
    });
    f.conn->connectEstablished();
    const std::string payload("before\0EOF", 10);
    sendPeer(f.pair.peer(), payload);
    check(::shutdown(f.pair.peer(), SHUT_WR) == 0, "peer half-close failed");
    f.loop.loop();
    check(received == payload && lifecycle == "UDC" && closed == 1 && io.reads >= 2,
          "EOF lost pending bytes or duplicated lifecycle callbacks");
    f.conn->connectDestroyed();
    f.conn->connectDestroyed();
    check(lifecycle == "UDC", "final cleanup duplicated down notification");
}

void test_read_error_classification() {
    for (int error : {EINTR, EAGAIN, EWOULDBLOCK, ECONNRESET, EIO, EBADF}) {
        Fixture f;
        const bool terminal = error != EINTR && error != EAGAIN && error != EWOULDBLOCK;
        std::string received;
        int closed = 0;
        f.conn->setMessageCallback([&](const TcpConnectionPtr&, Buffer* b, Timestamp) {
            received += b->retrieveAllAsString();
            f.loop.quit();
        });
        f.conn->setCloseCallback([&](const TcpConnectionPtr&) { ++closed; f.loop.quit(); });
        f.conn->connectEstablished();
        sendPeer(f.pair.peer(), "retry");
        io.readErrors = {error};
        LogCapture log;
        f.loop.loop();
        const std::string output = log.finish();
        check(io.nextReadError == 1, "read error injection did not reach connection");
        if (terminal) {
            check(f.conn->disconnected() && received.empty() && closed == 1 && io.reads == 1,
                  "terminal read error did not close exactly once");
            check(output.find("TcpConnection::handleRead [tcpconnection-test]") != std::string::npos &&
                  output.find("fd=" + std::to_string(f.fd)) != std::string::npos &&
                  output.find("errno=" + std::to_string(error)) != std::string::npos,
                  "read error log lost original errno or connection identity");
        } else {
            check(f.conn->connected() && received == "retry" && closed == 0 && io.reads >= 2 &&
                  output.empty(), "recoverable read error closed connection or prevented retry");
        }
    }
}

void test_send_overloads_and_callback_snapshot() {
    Fixture f;
    f.conn->connectEstablished();
    int completed = 0;
    f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr& c) {
        check(c == f.conn && f.loop.isInLoopTread(), "write completion owner changed");
        if (++completed == 3)
            f.loop.quit();
    });
    const char raw[] = {'r', '\0', 'w'};
    const std::string text("s\0t", 3);
    Buffer buffer;
    buffer.append("prefix", 6);
    buffer.retrieveAll();
    buffer.append("b\0f", 3);
    f.conn->send(raw, sizeof raw);
    f.conn->send(text);
    f.conn->send(&buffer);
    f.conn->send(nullptr, 0);
    f.conn->send(std::string());
    f.conn->send(&buffer);
    check(buffer.readableBytes() == 0 && completed == 0, "Buffer consumption or callback timing changed");
    f.conn->setWriteCompleteCallback({});
    check(receiveExactly(f.pair.peer(), 9) == std::string("r\0ws\0tb\0f", 9),
          "send overloads corrupted binary data or ordering");
    f.loop.loop();
    check(completed == 3, "empty send notified or queued callback lost its snapshot");
}

void test_cross_thread_send_owns_data_and_connection() {
    Fixture f;
    f.conn->connectEstablished();
    const std::weak_ptr<TcpConnection> weak = f.conn;
    int completed = 0;
    f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr& c) {
        check(f.loop.isInLoopTread(), "cross-thread completion ran outside loop");
        if (++completed == 3) {
            c->connectDestroyed();
            f.loop.quit();
        }
    });
    std::thread producer([conn = f.conn] {
        char raw[] = {'r', '\0', 'w'};
        conn->send(raw, sizeof raw);
        std::fill(raw, raw + sizeof raw, 'x');
        std::string text("s\0t", 3);
        conn->send(text);
        text.assign(4096, 'x');
        Buffer buffer;
        buffer.append("b\0f", 3);
        conn->send(&buffer);
        check(buffer.readableBytes() == 0, "cross-thread Buffer consumption was deferred");
        buffer.append("overwritten", 11);
    });
    producer.join();
    f.conn.reset();
    check(!weak.expired(), "queued send did not retain connection");
    f.loop.loop();
    check(completed == 3 && weak.expired(), "queued send leaked or lost connection ownership");
    check(receiveExactly(f.pair.peer(), 9) == std::string("r\0ws\0tb\0f", 9),
          "cross-thread send borrowed caller storage");
    check(fdClosed(f.fd), "queued callbacks retained fd after completion");
}

void test_buffered_send_order_and_retry() {
    {
        Fixture f;
        f.conn->connectEstablished();
        int completed = 0;
        f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; f.loop.quit(); });
        const std::string first = pattern(128 * 1024), second = pattern(96 * 1024);
        const std::string expected = first + second + "tail";
        Buffer tail;
        tail.append("tail", 4);
        io.shortSend = 7;
        f.conn->send(first);
        f.conn->send(second.data(), second.size());
        f.conn->send(&tail);
        std::string received;
        std::thread reader([&] { received = receiveExactly(f.pair.peer(), expected.size()); });
        f.loop.loop();
        reader.join();
        check(received == expected && completed == 1 && tail.readableBytes() == 0,
              "partial write or subsequent buffered send lost message ordering");
    }
    for (const auto& errors : {std::vector<int>{EINTR}, std::vector<int>{EAGAIN, EINTR, EAGAIN}}) {
        Fixture f;
        f.conn->connectEstablished();
        int completed = 0;
        f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; f.loop.quit(); });
        io.sendErrors = errors;
        f.conn->send("retry");
        f.loop.loop();
        check(completed == 1 && f.conn->connected() && io.nextSendError == errors.size() &&
              receiveExactly(f.pair.peer(), 5) == "retry", "retryable send error lost data or closed");
    }
}

void test_terminal_send_errors() {
    for (bool buffered : {false, true}) {
        Fixture f;
        f.conn->connectEstablished();
        int closed = 0, completed = 0;
        f.conn->setCloseCallback([&](const TcpConnectionPtr&) { ++closed; f.loop.quit(); });
        f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; });
        LogCapture log;
        if (buffered) {
            io.shortSend = 1;
            f.conn->send("buffered");
            io.sendErrors = {ECONNRESET};
            f.loop.loop();
        } else {
            f.pair.closePeer();
            f.conn->send("closed peer"); // 真实 EPIPE，保留默认 SIGPIPE 行为。
        }
        const std::string output = log.finish();
        check(f.conn->disconnected() && closed == 1 && completed == 0,
              "terminal write error did not close once without completion");
        check(output.find(buffered ? "TcpConnection::handleWrite" : "TcpConnection::sendInLoop")
                  != std::string::npos &&
              output.find("errno=" + std::to_string(buffered ? ECONNRESET : EPIPE)) != std::string::npos,
              "write error log lost failing operation or errno");
    }
}

void test_high_water_notification() {
    Fixture f;
    f.conn->connectEstablished();
    const std::weak_ptr<TcpConnection> weak = f.conn;
    const size_t mark = 64 * 1024 * 1024; // 验证公开行为，不修改私有阈值。
    int notifications = 0, completed = 0;
    f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; });
    f.conn->setHighWaterMarkCallback([&](const TcpConnectionPtr& c, size_t bytes) {
        check(f.loop.isInLoopTread() && bytes == mark, "high-water notification arguments changed");
        ++notifications;
        c->connectDestroyed();
        f.loop.quit();
    });
    io.blockSends = true;
    f.conn->send(std::string(mark, 'h'));
    f.conn->send("above threshold");
    check(notifications == 0, "high-water callback ran inline");
    f.conn->setHighWaterMarkCallback({});
    f.conn.reset();
    check(!weak.expired(), "queued high-water notification did not retain connection");
    f.loop.loop();
    check(notifications == 1 && completed == 0 && weak.expired(),
          "high-water notification repeated above threshold or lost callback snapshot");
}

void test_empty_shutdown_preserves_reading() {
    Fixture f;
    f.conn->connectEstablished();
    shutdownTcpConnectionInOtherTranslationUnit(f.conn);
    f.conn->shutdown();
    std::thread duplicate([conn = f.conn] { conn->shutdown(); });
    duplicate.join();
    char byte;
    check(io.shutdowns == 1 && ::read(f.pair.peer(), &byte, 1) == 0,
          "empty shutdown did not send FIN exactly once");
    check(!f.conn->connected() && !f.conn->disconnected(), "half-close marked connection disconnected");
    std::string received;
    f.conn->setMessageCallback([&](const TcpConnectionPtr&, Buffer* b, Timestamp) {
        received += b->retrieveAllAsString();
        f.loop.quit();
    });
    sendPeer(f.pair.peer(), "after FIN");
    f.loop.loop();
    check(received == "after FIN" && io.shutdowns == 1, "half-close disabled reading");
}

void test_graceful_shutdown_drains_output() {
    Fixture f;
    std::string lifecycle, received, reply;
    int completed = 0;
    f.conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
        lifecycle += c->connected() ? 'U' : 'D';
    });
    f.conn->setCloseCallback([&](const TcpConnectionPtr&) { lifecycle += 'C'; f.loop.quit(); });
    f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; });
    f.conn->setMessageCallback([&](const TcpConnectionPtr&, Buffer* b, Timestamp) {
        reply += b->retrieveAllAsString();
    });
    f.conn->connectEstablished();
    const std::string payload = pattern(512 * 1024);
    f.conn->send(payload);
    std::thread closer([conn = f.conn] { shutdownTcpConnectionInOtherTranslationUnit(conn); });
    closer.join();
    check(io.shutdowns == 0, "shutdown ran before output drained");
    f.conn->send("late");
    std::thread peer([&] {
        received = receiveUntilEof(f.pair.peer(), payload.size());
        sendPeer(f.pair.peer(), "reply");
        check(::shutdown(f.pair.peer(), SHUT_WR) == 0, "peer half-close failed");
    });
    f.loop.loop();
    peer.join();
    check(received == payload && reply == "reply" && completed == 1 && io.shutdowns == 1 &&
          lifecycle == "UDC" && f.conn->disconnected(), "graceful shutdown lost data or lifecycle");
}

void test_queued_send_and_stale_shutdown() {
    for (bool peerEof : {false, true}) {
        Fixture f;
        f.conn->connectEstablished();
        int completed = 0, closed = 0;
        f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; });
        f.conn->setCloseCallback([&](const TcpConnectionPtr&) { ++closed; f.loop.quit(); });
        std::thread producer([conn = f.conn] { conn->send("queued"); conn->shutdown(); });
        producer.join();
        if (peerEof) {
            check(::shutdown(f.pair.peer(), SHUT_WR) == 0, "peer half-close failed");
            f.loop.loop(); // 先处理 EOF，再处理已排队的发送和 shutdown。
            check(f.conn->disconnected() && closed == 1 && io.shutdowns == 0,
                  "stale shutdown touched a disconnected socket");
        } else {
            f.runPending();
            char byte;
            check(::read(f.pair.peer(), &byte, 1) == 0 && io.shutdowns == 1 && closed == 0,
                  "queued send was accepted after shutdown began");
        }
        check(completed == 0, "discarded queued send reported completion");
    }
}

void test_force_close_discards_pending_output() {
    for (bool crossThread : {false, true}) {
        for (bool afterShutdown : {false, true}) {
            Fixture f;
            std::string lifecycle;
            int completed = 0;
            const std::weak_ptr<TcpConnection> weak = f.conn;
            f.conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
                lifecycle += c->connected() ? 'U' : 'D';
            });
            f.conn->setWriteCompleteCallback([&](const TcpConnectionPtr&) { ++completed; });
            f.conn->setCloseCallback([&](const TcpConnectionPtr& c) {
                check(f.loop.isInLoopTread() && !weak.expired(), "forceClose lost owner thread or lifetime");
                lifecycle += 'C';
                c->connectDestroyed();
                f.loop.quit();
            });
            f.conn->connectEstablished();
            const size_t length = 512 * 1024;
            f.conn->send(std::string(length, 'f'));
            if (afterShutdown)
                f.conn->shutdown();
            {
                auto close = [conn = f.conn] {
                    forceCloseTcpConnectionInOtherTranslationUnit(conn);
                    conn->forceClose();
                };
                if (crossThread) {
                    std::thread closer(close);
                    closer.join();
                } else {
                    close();
                }
            }
            check(lifecycle == "U", "forceClose ran inline");
            f.conn.reset();
            check(!weak.expired(), "forceClose task did not retain connection");
            f.loop.loop();
            check(lifecycle == "UDC" && completed == 0 && io.shutdowns == 0 && weak.expired() &&
                  fdClosed(f.fd), "forceClose waited for output, leaked, or notified twice");
            check(receiveUntilEof(f.pair.peer(), length).size() < length,
                  "forceClose unexpectedly drained all output");
        }
    }
}

void test_callbacks_release_last_owner() {
    for (bool fromReadEvent : {false, true}) {
        Fixture f;
        const std::weak_ptr<TcpConnection> weak = f.conn;
        int down = 0;
        f.conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
            if (c->disconnected()) {
                ++down;
                f.conn.reset();
                check(!weak.expired(), "down callback lost connection before cleanup finished");
            }
        });
        if (fromReadEvent) {
            f.conn->setMessageCallback([&](const TcpConnectionPtr& c, Buffer* b, Timestamp) {
                check(b->retrieveAllAsString() == "x", "unexpected tie test payload");
                c->connectDestroyed();
                f.loop.queueInLoop([&] {
                    check(weak.expired() && fdClosed(f.fd), "Channel tie retained connection after dispatch");
                    f.loop.quit();
                });
            });
        }
        f.conn->connectEstablished();
        if (fromReadEvent) {
            sendPeer(f.pair.peer(), "x");
            f.loop.loop(); // tie 应保护 Channel，直到 handleEvent 整体返回。
        } else {
            f.conn->connectDestroyed();
        }
        check(down == 1 && weak.expired() && fdClosed(f.fd), "callback release leaked connection or fd");
    }
}

void test_concurrent_close_keeps_terminal_state() {
    for (int iteration = 0; iteration < 16; ++iteration) {
        Fixture f;
        int down = 0, closed = 0;
        f.conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
            if (c->disconnected())
                ++down;
        });
        f.conn->setCloseCallback([&](const TcpConnectionPtr&) { ++closed; });
        f.conn->connectEstablished();
        std::atomic_bool start{false};
        std::vector<std::thread> closers;
        for (int i = 0; i < 4; ++i) {
            closers.emplace_back([&, conn = f.conn, i] {
                while (!start.load())
                    std::this_thread::yield();
                if (i % 2)
                    conn->shutdown();
                else
                    conn->forceClose();
            });
        }
        start.store(true);
        f.conn->connectDestroyed();
        for (auto& closer : closers)
            closer.join();
        f.runPending();
        check(f.conn->disconnected() && down == 1 && closed == 0 && io.shutdowns == 0,
              "concurrent close resurrected state or executed stale tasks");
    }
}
} // namespace

int main() {
    Logger::setLevel(LogLevel::ERROR);
    const struct {
        const char* name;
        void (*run)();
    } tests[] = {
        {"creation_and_optional_callbacks", test_creation_and_optional_callbacks},
        {"read_binary_timestamp_and_eof", test_read_binary_timestamp_and_eof},
        {"read_error_classification", test_read_error_classification},
        {"send_overloads_and_callback_snapshot", test_send_overloads_and_callback_snapshot},
        {"cross_thread_send_ownership", test_cross_thread_send_owns_data_and_connection},
        {"buffered_send_order_and_retry", test_buffered_send_order_and_retry},
        {"terminal_send_errors", test_terminal_send_errors},
        {"high_water_notification", test_high_water_notification},
        {"empty_shutdown_preserves_reading", test_empty_shutdown_preserves_reading},
        {"graceful_shutdown_drains_output", test_graceful_shutdown_drains_output},
        {"queued_send_and_stale_shutdown", test_queued_send_and_stale_shutdown},
        {"force_close_discards_pending_output", test_force_close_discards_pending_output},
        {"callbacks_release_last_owner", test_callbacks_release_last_owner},
        {"concurrent_close_keeps_terminal_state", test_concurrent_close_keeps_terminal_state},
    };
    for (const auto& test : tests) {
        currentTest = test.name;
        std::cout << "[RUN] " << currentTest << std::endl;
        test.run();
        std::cout << "[PASS] " << currentTest << '\n';
    }
    std::cout << "test_tcpconnection: all passed\n";
}
