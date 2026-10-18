#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netlib/socket.h"

using namespace netlib;

std::unique_ptr<Socket> makeSocketInOtherTranslationUnit(int fd);
int acceptInOtherTranslationUnit(Socket& socket, InetAddress* peeraddr);

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_socket failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static int createTcpFd(bool nonblocking = true) {
    const int flags = SOCK_STREAM | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0);
    const int fd = ::socket(AF_INET, flags, IPPROTO_TCP);
    check(fd >= 0, "socket failed");
    return fd;
}

static int closedTcpFd() {
    const int fd = createTcpFd();
    check(::close(fd) == 0, "close failed");
    return fd;
}

static sockaddr_in localAddress(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    check(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
          "getsockname failed");
    check(len == sizeof addr && addr.sin_family == AF_INET, "invalid local address");
    return addr;
}

static void waitFor(int fd, short events) {
    pollfd entry{fd, events, 0};
    int ready;
    do {
        ready = ::poll(&entry, 1, 2000);
    } while (ready < 0 && errno == EINTR);
    check(ready == 1 && (entry.revents & (events | POLLHUP)), "socket readiness timed out");
}

static void sendBytes(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        waitFor(fd, POLLOUT);
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "send failed");
        sent += static_cast<std::size_t>(n);
    }
}

static std::string readBytes(int fd, std::size_t length) {
    std::string data(length, '\0');
    std::size_t received = 0;
    while (received < length) {
        waitFor(fd, POLLIN);
        const ssize_t n = ::read(fd, data.data() + received, length - received);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(n > 0, "read failed or reached EOF before all data arrived");
        received += static_cast<std::size_t>(n);
    }
    return data;
}

class StderrCapture : private Noncopyable {
public:
    StderrCapture() {
        std::fflush(stderr);
        file_ = std::tmpfile();
        check(file_ != nullptr, "tmpfile failed");
        savedFd_ = ::dup(STDERR_FILENO);
        check(savedFd_ >= 0, "dup failed");
        check(::dup2(::fileno(file_), STDERR_FILENO) >= 0, "dup2 failed");
    }
    ~StderrCapture() {
        std::fflush(stderr);
        ::dup2(savedFd_, STDERR_FILENO);
        ::close(savedFd_);
        std::fclose(file_);
    }
    std::string output() {
        std::fflush(stderr);
        check(std::fseek(file_, 0, SEEK_SET) == 0, "fseek failed");
        std::string result;
        char buffer[1024];
        std::size_t count;
        while ((count = std::fread(buffer, 1, sizeof buffer, file_)) > 0)
            result.append(buffer, count);
        check(!std::ferror(file_), "fread failed");
        return result;
    }
private:
    std::FILE* file_ = nullptr;
    int savedFd_ = -1;
};

template <typename Callback>
static std::string expectAbort(Callback callback) {
    StderrCapture capture;
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
          "invalid input or fatal syscall failure did not abort");
    return capture.output();
}

static void test_fd_ownership() {
    const int fd = createTcpFd();
    {
        auto socket = makeSocketInOtherTranslationUnit(fd);
        check(socket->fd() == fd && ::fcntl(fd, F_GETFD) >= 0, "socket did not own supplied fd");
    }
    const int result = ::fcntl(fd, F_GETFD);
    const int savedErrno = errno;
    check(result == -1 && savedErrno == EBADF, "destructor did not close fd");
}

struct SocketOption {
    int level;
    int option;
    const char* name;
    void (Socket::*set)(bool);
};

static const SocketOption options[] = {
    {IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY", &Socket::setTcpNoDelay},
    {SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR", &Socket::setReuseAddr},
    {SOL_SOCKET, SO_REUSEPORT, "SO_REUSEPORT", &Socket::setReusePort},
    {SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE", &Socket::setKeepAlive},
};

static void test_socket_options() {
    Socket socket(createTcpFd());
    for (const auto& option : options) {
        for (bool enabled : {true, false}) {
            (socket.*option.set)(enabled);
            int value = -1;
            socklen_t len = sizeof value;
            check(::getsockopt(socket.fd(), option.level, option.option, &value, &len) == 0,
                  "getsockopt failed");
            check(len == sizeof value && value == static_cast<int>(enabled),
                  "socket option was not enabled or disabled");
        }
    }
}

static void test_accept_and_half_close() {
    Socket listener(createTcpFd());
    listener.setReuseAddr(true);
    listener.bindAddress(InetAddress("127.0.0.1", 0));
    listener.listen();
    const sockaddr_in address = localAddress(listener.fd());

    InetAddress peer("192.0.2.1", 12345);
    const std::string unchanged = peer.toIpPort();
    const int empty = listener.accept(&peer);
    const int savedErrno = errno;
    check(empty == -1 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK),
          "empty nonblocking accept did not preserve EAGAIN");
    check(peer.toIpPort() == unchanged, "failed accept modified peer address");

    Socket client(createTcpFd(false));
    check(::connect(client.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0,
          "loopback connect failed");
    const sockaddr_in clientAddress = localAddress(client.fd());
    waitFor(listener.fd(), POLLIN);
    const int acceptedFd = acceptInOtherTranslationUnit(listener, &peer);
    check(acceptedFd >= 0, "accept failed");
    Socket connection(acceptedFd);
    const int statusFlags = ::fcntl(connection.fd(), F_GETFL);
    const int descriptorFlags = ::fcntl(connection.fd(), F_GETFD);
    check(statusFlags >= 0 && (statusFlags & O_NONBLOCK), "accepted fd is blocking");
    check(descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC), "accepted fd lacks CLOEXEC");
    check(peer.ip() == "127.0.0.1" && peer.port() == ::ntohs(clientAddress.sin_port)
          && peer.size() == sizeof(sockaddr_in), "accept returned an incorrect peer address");

    sendBytes(connection.fd(), "reply");
    connection.shutdownWrite();
    check(readBytes(client.fd(), 5) == "reply", "shutdownWrite discarded queued bytes");
    waitFor(client.fd(), POLLIN);
    char byte;
    check(::read(client.fd(), &byte, 1) == 0, "shutdownWrite did not produce peer EOF");
    sendBytes(client.fd(), "still-readable");
    check(readBytes(connection.fd(), 14) == "still-readable", "shutdownWrite closed read side");
}

static void test_error_reporting() {
    StderrCapture capture;
    // 用已关闭的 fd 触发真实系统调用错误，捕获日志的文件在关闭 fd 前已打开。
    Socket invalid(closedTcpFd());
    for (const auto& option : options) {
        (invalid.*option.set)(true);
        check(errno == EBADF, "setsockopt error logging changed errno");
    }
    invalid.shutdownWrite();
    check(errno == EBADF, "shutdownWrite error logging changed errno");
    InetAddress peer("192.0.2.2", 54321);
    const std::string unchanged = peer.toIpPort();
    const int acceptedFd = invalid.accept(&peer);
    const int savedErrno = errno;
    check(acceptedFd == -1 && savedErrno == EBADF && peer.toIpPort() == unchanged,
          "accept did not preserve EBADF or the original peer address");

    const std::string output = capture.output();
    for (const auto& option : options) {
        const std::string message = "setsockopt fd=" + std::to_string(invalid.fd())
            + " option=" + option.name + " errno=" + std::to_string(EBADF);
        check(output.find(message) != std::string::npos, "setsockopt error details missing");
    }
    const std::string message = "shutdownWrite fd=" + std::to_string(invalid.fd())
        + " errno=" + std::to_string(EBADF);
    check(output.find(message) != std::string::npos, "shutdownWrite error details missing");
}

static void test_fatal_errors_and_preconditions() {
    const std::string bindError = expectAbort([] {
        Socket invalid(closedTcpFd());
        invalid.bindAddress(InetAddress("127.0.0.1", 0));
    });
    check(bindError.find("bind fd=") != std::string::npos
          && bindError.find("errno=" + std::to_string(EBADF)) != std::string::npos,
          "fatal bind log lacks operation, fd or errno");
    const std::string listenError = expectAbort([] {
        Socket invalid(closedTcpFd());
        invalid.listen();
    });
    check(listenError.find("listen fd=") != std::string::npos
          && listenError.find("errno=" + std::to_string(EBADF)) != std::string::npos,
          "fatal listen log lacks operation, fd or errno");
    expectAbort([] { Socket invalid(-1); });
    expectAbort([] {
        Socket listener(createTcpFd());
        listener.accept(nullptr);
    });
}

int main() {
    Logger::setLevel(LogLevel::WARN);
    test_fd_ownership();
    test_socket_options();
    test_accept_and_half_close();
    test_error_reporting();
    test_fatal_errors_and_preconditions();
    std::cout << "test_socket: all passed\n";
}
