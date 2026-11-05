#include "../examples/echo_server.h"
#include "../examples/echo_server.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/uio.h>
#include <thread>
#include <vector>

using namespace netlib;
using namespace netlib::example;
using namespace std::chrono_literals;

namespace {
std::atomic_int observedFd{-1};
std::atomic_size_t inputBytes{0}, outputBytes{0}, shortWrites{0}, wouldBlock{0};
std::atomic_bool sawEof{false};
int signalFd = -1;
bool failSignalFd = false, failSignalRegistration = false;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_echo_support: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __wrap_accept4(int fd, sockaddr* address, socklen_t* size, int flags) {
    const int result = __real_accept4(fd, address, size, flags);
    if (result >= 0) {
        const int buffer = 4096;
        check(::setsockopt(result, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer) == 0,
              "could not constrain accepted socket send buffer");
        inputBytes = outputBytes = shortWrites = wouldBlock = 0;
        sawEof = false;
        observedFd = result;
    }
    return result;
}
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t length, int flags) {
    const ssize_t result = __real_send(fd, data, length, flags);
    const int error = errno;
    if (fd == observedFd.load()) {
        if (result >= 0) {
            outputBytes += static_cast<size_t>(result);
            if (static_cast<size_t>(result) < length)
                ++shortWrites;
        } else if (error == EAGAIN || error == EWOULDBLOCK) {
            ++wouldBlock;
        }
    }
    errno = error;
    return result;
}
extern "C" ssize_t __real_readv(int, const iovec*, int);
extern "C" ssize_t __wrap_readv(int fd, const iovec* vectors, int size) {
    const ssize_t result = __real_readv(fd, vectors, size);
    const int error = errno;
    if (fd == observedFd.load()) {
        if (result > 0)
            inputBytes += static_cast<size_t>(result);
        else if (result == 0)
            sawEof = true;
    }
    errno = error;
    return result;
}
extern "C" int __real_signalfd(int, const sigset_t*, int);
extern "C" int __wrap_signalfd(int fd, const sigset_t* mask, int flags) {
    if (failSignalFd) {
        errno = EMFILE;
        return -1;
    }
    signalFd = __real_signalfd(fd, mask, flags);
    return signalFd;
}
extern "C" int __real_epoll_ctl(int, int, int, epoll_event*);
extern "C" int __wrap_epoll_ctl(int epoll, int operation, int fd, epoll_event* event) {
    if (failSignalRegistration && fd == signalFd && operation == EPOLL_CTL_ADD) {
        errno = EIO;
        return -1;
    }
    return __real_epoll_ctl(epoll, operation, fd, event);
}

namespace {
sigset_t currentMask() {
    sigset_t mask{};
    check(::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0, "could not read signal mask");
    return mask;
}
void checkMask(const sigset_t& expected) {
    const auto actual = currentMask();
    for (int signal = 1; signal < NSIG; ++signal)
        check(::sigismember(&expected, signal) == ::sigismember(&actual, signal),
              "signal mask was not restored");
}
int countEntries(const char* path) {
    DIR* directory = ::opendir(path);
    check(directory != nullptr, "could not inspect process resources");
    int count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] != '.')
            ++count;
    ::closedir(directory);
    return count;
}
void waitFor(int fd, short events) {
    pollfd entry{fd, events, 0};
    int result;
    do { result = ::poll(&entry, 1, 3000); } while (result < 0 && errno == EINTR);
    check(result == 1 && (entry.revents & (events | POLLHUP)), "socket readiness timed out");
}
std::unique_ptr<Socket> connectClient(const InetAddress& address) {
    auto socket = Socket::adopt(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const int buffer = 65536;
    check(::setsockopt(socket->fd(), SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer) == 0,
          "could not constrain client receive buffer");
    check(::connect(socket->fd(), reinterpret_cast<const sockaddr*>(&address.addr()),
                    address.size()) == 0, "could not connect to advertised port");
    const int flags = ::fcntl(socket->fd(), F_GETFL);
    check(flags >= 0 && ::fcntl(socket->fd(), F_SETFL, flags | O_NONBLOCK) == 0,
          "could not make client nonblocking");
    return socket;
}
void sendAll(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        waitFor(fd, POLLOUT);
        const ssize_t size = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        check(size > 0, "client send failed");
        offset += static_cast<size_t>(size);
    }
}
std::string receive(int fd, size_t length, bool untilEof = false) {
    std::string bytes;
    while (untilEof || bytes.size() < length) {
        waitFor(fd, POLLIN);
        char data[16384];
        const ssize_t size = ::read(fd, data, sizeof data);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (untilEof && size == 0)
            break;
        check(size > 0, "unexpected receive failure or early EOF");
        bytes.append(data, static_cast<size_t>(size));
        check(bytes.size() <= length, "received extra bytes");
    }
    return bytes;
}

void testOptions() {
    const char* defaults[]{"echo_server"};
    const auto options = parseEchoOptions(1, defaults);
    check(options.port == 12300 && options.threads == 3 && !options.help, "defaults changed");
    const char* custom[]{"echo_server", "--port", "0", "--threads", "0", "--help"};
    const auto configured = parseEchoOptions(6, custom);
    check(configured.port == 0 && configured.threads == 0 && configured.help,
          "ephemeral port, zero workers, or help was rejected");
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"echo_server", "--port"}, {"echo_server", "--threads"},
             {"echo_server", "--unknown"}, {"echo_server", "--port", "65536"},
             {"echo_server", "--port", "-1"}, {"echo_server", "--threads", "-1"},
             {"echo_server", "--threads", "2147483648"}, {"echo_server", "--port", "12x"},
             {"echo_server", "--port", ""}, {"echo_server", "--threads", "+1"}}) {
        bool rejected = false;
        try { parseEchoOptions(static_cast<int>(args.size()), args.data()); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid command line was accepted");
    }
}

void testSignalSetupFailure() {
    const auto before = currentMask();
    const int fds = countEntries("/proc/self/fd");
    {
        EventLoop loop;
        for (bool failRegistration : {false, true}) {
            failSignalFd = !failRegistration;
            failSignalRegistration = failRegistration;
            bool rejected = false;
            try { SignalStop stop(&loop); }
            catch (const std::system_error& error) {
                rejected = error.code().value() == (failRegistration ? EIO : EMFILE);
            }
            failSignalFd = failSignalRegistration = false;
            check(rejected, "signal setup did not preserve its original error");
            checkMask(before);
            if (failRegistration)
                check(::fcntl(signalFd, F_GETFD) < 0 && errno == EBADF,
                      "failed signal registration leaked fd");
        }
    }
    check(countEntries("/proc/self/fd") == fds, "failed setup leaked resources");
}

struct ReadyBuffer : std::stringbuf {
    int flushes = 0;
    int sync() override { ++flushes; return std::stringbuf::sync(); }
};

void testEchoAndStop(int workers, int stopSignal) {
    const auto before = currentMask();
    const int fds = countEntries("/proc/self/fd"), tasks = countEntries("/proc/self/task");
    std::thread client;
    {
        EventLoop loop;
        SignalStop stop(&loop);
        const auto blocked = currentMask();
        check(::sigismember(&blocked, SIGINT) == 1 && ::sigismember(&blocked, SIGTERM) == 1,
              "stop signals were not blocked before worker creation");
        check((::fcntl(signalFd, F_GETFL) & O_NONBLOCK) != 0 &&
              (::fcntl(signalFd, F_GETFD) & FD_CLOEXEC) != 0, "signalfd flags missing");
        EchoServer server(&loop, InetAddress("127.0.0.1", 0), "echo-support-test", workers);
        server.start();
        ReadyBuffer buffer;
        std::ostream output(&buffer);
        server.printReady(output);
        std::string tag, endpoint;
        std::istringstream ready(buffer.str());
        ready >> tag >> endpoint;
        check(tag == "READY" && buffer.flushes == 1, "ready notification was not flushed");
        const auto separator = endpoint.rfind(':');
        check(separator != std::string::npos, "ready notification has no port");
        const auto port = std::stoi(endpoint.substr(separator + 1));
        check(port > 0 && port <= 65535, "ready notification advertised port zero");
        const InetAddress address(endpoint.substr(0, separator), static_cast<uint16_t>(port));
        if (stopSignal == SIGINT) {
            // 模拟就绪后、loop.loop() 之前到达的停止信号。
            check(::kill(::getpid(), stopSignal) == 0, "could not request idle stop");
        } else {
            client = std::thread([&, address] {
                auto active = connectClient(address);
                const std::string text = u8"难过，先人🙂";
                for (int round = 0; round < 3; ++round) {
                    sendAll(active->fd(), text);
                    check(receive(active->fd(), text.size()) == text, "persistent UTF-8 echo changed");
                }
                auto slow = connectClient(address);
                std::string payload(4 * 1024 * 1024, '\0');
                for (size_t i = 0; i < payload.size(); ++i)
                    payload[i] = static_cast<char>(i % 251);
                sendAll(slow->fd(), payload);
                check(::shutdown(slow->fd(), SHUT_WR) == 0, "client half-close failed");
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (!sawEof.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                check(sawEof && inputBytes == payload.size() && outputBytes < payload.size() &&
                      (shortWrites > 0 || wouldBlock > 0), "test did not produce real EOF/backpressure");
                check(receive(slow->fd(), payload.size(), true) == payload,
                      "slow-reader half-close truncated binary echo");
                sendAll(active->fd(), text);
                check(receive(active->fd(), text.size()) == text, "server stopped accepting messages");
                check(::kill(::getpid(), stopSignal) == 0, "could not request active stop");
                check(receive(active->fd(), 0, true).empty(), "active connection was not cleaned up");
            });
        }
        loop.loop(); // 信号只请求退出；在作用域结束时按顺序析构 Server、SignalStop、loop。
    }
    if (client.joinable())
        client.join();
    observedFd = -1;
    checkMask(before);
    check(countEntries("/proc/self/fd") == fds, "normal stop leaked fd");
    check(countEntries("/proc/self/task") == tasks, "normal stop left worker threads alive");
    std::cout << "[PASS] workers=" << workers << " signal=" << stopSignal << '\n';
}
} // namespace

int main() {
    Logger::setLevel(LogLevel::ERROR);
    testOptions();
    testSignalSetupFailure();
    for (int workers : {0, 1, 3}) {
        testEchoAndStop(workers, SIGINT);
        testEchoAndStop(workers, SIGTERM);
    }
    std::cout << "test_echo_support: all passed\n";
}
