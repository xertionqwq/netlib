#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netlib/acceptor.h"

using namespace netlib;

std::unique_ptr<Acceptor> makeAcceptorInOtherTranslationUnit(
    EventLoop* loop, const InetAddress& address, bool reuseport);
void listenInOtherTranslationUnit(Acceptor& acceptor);

static int lastSocketFd = -1;
static int socketCalls = 0;
static int nextSocketError = 0;
static int watchedFd = -1;
static int watchedUpdates = 0;
static bool failNextAdd = false;
static int nextAcceptError = 0;
static int lastAcceptedFd = -1;
static bool quitAfterAccept = false;

extern "C" int __real_socket(int, int, int);
extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event*);

extern "C" int __wrap_socket(int domain, int type, int protocol) {
    ++socketCalls;
    if (nextSocketError != 0) {
        errno = std::exchange(nextSocketError, 0);
        return -1;
    }
    lastSocketFd = __real_socket(domain, type, protocol);
    return lastSocketFd;
}

extern "C" int __wrap_epoll_ctl(int epfd, int operation, int fd, epoll_event* event) {
    if (fd == watchedFd) {
        ++watchedUpdates;
        if (operation == EPOLL_CTL_ADD && std::exchange(failNextAdd, false)) {
            errno = EIO;
            return -1;
        }
    }
    return __real_epoll_ctl(epfd, operation, fd, event);
}

// 真实 epoll 和本机连接触发接入；仅对单次系统调用失败进行注入。
extern "C" int __wrap_accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    int result;
    if (nextAcceptError != 0) {
        errno = std::exchange(nextAcceptError, 0);
        result = -1;
    } else {
        result = __real_accept4(fd, address, length, flags);
    }
    const int savedErrno = errno;
    lastAcceptedFd = result;
    if (std::exchange(quitAfterAccept, false)) {
        EventLoop* loop = loopPtr;
        loop->queueInLoop([loop] { loop->quit(); });
    }
    errno = savedErrno;
    return result;
}

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_acceptor failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static void checkClosed(int fd) {
    const int result = ::fcntl(fd, F_GETFD);
    const int savedErrno = errno;
    check(result == -1 && savedErrno == EBADF, "fd was not closed");
}

static int socketOption(int fd, int option) {
    int value = -1;
    socklen_t length = sizeof value;
    check(::getsockopt(fd, SOL_SOCKET, option, &value, &length) == 0, "getsockopt failed");
    return value;
}

static sockaddr_in localAddress(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof address;
    check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "getsockname failed");
    check(length == sizeof address && address.sin_family == AF_INET, "invalid IPv4 address");
    return address;
}

static std::unique_ptr<Socket> connectClient(int listenerFd) {
    const auto address = localAddress(listenerFd);
    // 客户端不经过 socket 故障注入，lastSocketFd 始终指向 Acceptor 的监听 fd。
    const int fd = __real_socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    check(fd >= 0, "client socket failed");
    auto client = std::make_unique<Socket>(fd);
    check(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0,
          "loopback connection failed");
    return client;
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
        std::size_t length;
        while ((length = std::fread(buffer, 1, sizeof buffer, file_)) > 0)
            result.append(buffer, length);
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
          "thread violation or fatal socket error did not abort");
    return capture.output();
}

static void test_construction_and_cleanup() {
    const int before = socketCalls;
    bool rejected = false;
    try {
        Acceptor invalid(nullptr, InetAddress("127.0.0.1", 0));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && socketCalls == before, "null loop was not rejected before fd allocation");

    EventLoop loop;
    for (bool reuseport : {false, true}) {
        int fd;
        {
            auto acceptor = makeAcceptorInOtherTranslationUnit(
                &loop, InetAddress("127.0.0.1", 0), reuseport);
            fd = lastSocketFd;
            check(!acceptor->listening() && socketOption(fd, SO_ACCEPTCONN) == 0,
                  "construction started listening");
            check(localAddress(fd).sin_port != 0, "construction did not bind an address");
            check(socketOption(fd, SO_REUSEADDR) == 1
                  && socketOption(fd, SO_REUSEPORT) == static_cast<int>(reuseport),
                  "address reuse options were not applied");
            const int flags = ::fcntl(fd, F_GETFL);
            const int fdFlags = ::fcntl(fd, F_GETFD);
            check(flags >= 0 && (flags & O_NONBLOCK) && fdFlags >= 0 && (fdFlags & FD_CLOEXEC),
                  "listener lacks NONBLOCK or CLOEXEC");
        }
        checkClosed(fd);
    }
}

static void test_registration_retry_and_handoff() {
    EventLoop loop(-1);
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
    const int listenerFd = lastSocketFd;
    watchedFd = listenerFd;
    watchedUpdates = 0;
    failNextAdd = true;
    bool caught = false;
    {
        StderrCapture capture;
        try {
            acceptor.listen();
        } catch (const std::system_error& error) {
            caught = error.code().value() == EIO;
        }
    }
    check(caught && !failNextAdd && !acceptor.listening(),
          "failed event registration was reported as successful startup");

    std::unique_ptr<Socket> accepted;
    InetAddress receivedPeer(0);
    Acceptor::NewConnectionCallback callback = [&](int fd, const InetAddress& peer) {
        accepted = std::make_unique<Socket>(fd);
        receivedPeer = peer;
        loop.quit();
    };
    acceptor.setNewConnectionCallback(callback); // 已有 std::function 左值也能传入。
    listenInOtherTranslationUnit(acceptor);
    check(acceptor.listening(), "startup retry failed");
    const int updates = watchedUpdates;
    acceptor.listen();
    check(watchedUpdates == updates, "repeated listen updated event registration");
    auto client = connectClient(listenerFd);
    const auto clientAddress = localAddress(client->fd());
    loop.loop();
    check(accepted != nullptr, "connection callback was not invoked");
    const int acceptedFd = accepted->fd();
    const int flags = ::fcntl(acceptedFd, F_GETFL);
    const int fdFlags = ::fcntl(acceptedFd, F_GETFD);
    check(flags >= 0 && (flags & O_NONBLOCK) && fdFlags >= 0 && (fdFlags & FD_CLOEXEC),
          "handed-off connection lacks NONBLOCK or CLOEXEC");
    check(receivedPeer.ip() == "127.0.0.1" && receivedPeer.port() == ::ntohs(clientAddress.sin_port),
          "callback received an incorrect peer address");
    accepted.reset();
    checkClosed(acceptedFd);
    watchedFd = -1;
}

static void test_missing_callback_closes_connection() {
    EventLoop loop(-1);
    int listenerFd;
    {
        Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
        listenerFd = lastSocketFd;
        acceptor.listen();
        auto client = connectClient(listenerFd);
        quitAfterAccept = true;
        loop.loop();
        check(lastAcceptedFd >= 0, "no connection was accepted");
        checkClosed(lastAcceptedFd);
        pollfd entry{client->fd(), POLLIN, 0};
        check(::poll(&entry, 1, 2000) == 1, "client did not observe connection closure");
        char byte;
        check(::read(client->fd(), &byte, 1) == 0, "missing callback did not close connection");
    }
    checkClosed(listenerFd);
}

static void test_accept_errors() {
    for (int error : {EAGAIN, EINTR, EMFILE, EBADF}) {
        StderrCapture capture;
        EventLoop loop(-1);
        Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
        const int listenerFd = lastSocketFd;
        int callbacks = 0;
        acceptor.setNewConnectionCallback([&](int fd, const InetAddress&) {
            Socket owned(fd);
            ++callbacks;
        });
        acceptor.listen();
        auto client = connectClient(listenerFd);
        nextAcceptError = error;
        quitAfterAccept = true;
        loop.loop();
        check(nextAcceptError == 0 && callbacks == 0, "injected accept error was not handled");
        const auto output = capture.output();
        if (error == EAGAIN || error == EINTR) {
            check(output.empty(), "normal accept condition produced an error log");
        } else {
            const std::string expected = "Acceptor::handleRead accept fd="
                + std::to_string(listenerFd) + " errno=" + std::to_string(error);
            check(output.find(expected) != std::string::npos, "accept log lost fd or original errno");
            if (error == EMFILE)
                check(output.find("socket fd limit reached") != std::string::npos,
                      "fd exhaustion was not identified");
        }
    }
}

static void test_callback_exception_ownership() {
    for (bool reuseFd : {false, true}) {
        EventLoop loop(-1);
        Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
        acceptor.listen();
        auto client = connectClient(lastSocketFd);
        int acceptedFd = -1;
        acceptor.setNewConnectionCallback([&](int fd, const InetAddress&) {
            acceptedFd = fd;
            if (reuseFd) {
                { Socket owned(fd); }
                check(::dup2(client->fd(), fd) == fd, "could not reuse the callback-owned fd");
            } else {
                Socket owned(fd);
                throw std::runtime_error("connection callback failure");
            }
            throw std::runtime_error("connection callback failure");
        });
        bool caught = false;
        try {
            loop.loop();
        } catch (const std::runtime_error& error) {
            caught = std::string(error.what()) == "connection callback failure";
        }
        check(caught && acceptedFd >= 0, "connection callback exception did not propagate");
        if (reuseFd) {
            check(::fcntl(acceptedFd, F_GETFD) >= 0, "Acceptor closed a callback-owned reused fd");
            ::close(acceptedFd);
        } else {
            checkClosed(acceptedFd);
        }
    }
}

static void test_socket_creation_failure() {
    const auto output = expectAbort([] {
        nextSocketError = EMFILE;
        Acceptor::createNonblockingOrDie();
    });
    check(output.find("listen socket create errno=" + std::to_string(EMFILE)) != std::string::npos,
          "socket creation failure lost its original errno");
}

static void test_thread_affinity() {
    expectAbort([] {
        EventLoop loop;
        std::thread worker([&] { Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0)); });
        worker.join();
    });
    for (int operation : {0, 1, 2}) {
        expectAbort([=] {
            EventLoop loop;
            Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
            std::thread worker([&] {
                if (operation == 0)
                    acceptor.setNewConnectionCallback({});
                else if (operation == 1)
                    acceptor.listen();
                else
                    acceptor.listening();
            });
            worker.join();
        });
    }
    expectAbort([] {
        EventLoop loop;
        auto acceptor = std::make_unique<Acceptor>(&loop, InetAddress("127.0.0.1", 0));
        std::thread worker([acceptor = std::move(acceptor)]() mutable { acceptor.reset(); });
        worker.join();
    });
}

int main() {
    Logger::setLevel(LogLevel::WARN);
    test_construction_and_cleanup();
    test_registration_retry_and_handoff();
    test_missing_callback_closes_connection();
    test_accept_errors();
    test_callback_exception_ownership();
    test_socket_creation_failure();
    test_thread_affinity();
    std::cout << "test_acceptor: all passed\n";
}
