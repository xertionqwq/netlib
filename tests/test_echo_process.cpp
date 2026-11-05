// 黑盒验证正式 echo_server；不包含 EchoServer，也不包装服务端系统调用。
#include "netlib/noncopyable.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;
Time suiteDeadline;
std::string currentCase = "setup";

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void checkError(int error, const char* operation) {
    if (error != 0)
        throw std::system_error(error, std::generic_category(), operation);
}
bool retryIo() { return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK; }
Time deadlineAfter(std::chrono::seconds duration) {
    return std::min(suiteDeadline, Clock::now() + duration);
}

class Fd : private netlib::Noncopyable {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other)
            reset(std::exchange(other.fd_, -1));
        return *this;
    }
    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
private:
    int fd_;
};

struct Pipe {
    Fd read_, write_;
    Pipe() {
        int descriptors[2];
        if (::pipe2(descriptors, O_CLOEXEC) < 0)
            checkError(errno, "pipe2");
        read_.reset(descriptors[0]);
        write_.reset(descriptors[1]);
        // 即使父进程关闭了 stdin，管道也不能占用将被重定向的 0/1/2。
        for (Fd* endpoint : {&read_, &write_}) {
            if (endpoint->get() <= STDERR_FILENO) {
                const int copy = ::fcntl(endpoint->get(), F_DUPFD_CLOEXEC, 3);
                if (copy < 0)
                    checkError(errno, "duplicate pipe fd");
                endpoint->reset(copy);
            }
        }
        const int flags = ::fcntl(read_.get(), F_GETFL);
        if (flags < 0 || ::fcntl(read_.get(), F_SETFL, flags | O_NONBLOCK) < 0)
            checkError(errno, "make pipe reader nonblocking");
        // 写端保持阻塞，和从终端/普通管道启动程序时的语义一致。
    }
};

struct SpawnSetup : private netlib::Noncopyable {
    posix_spawn_file_actions_t actions_{};
    posix_spawnattr_t attributes_{};
    SpawnSetup() {
        checkError(::posix_spawn_file_actions_init(&actions_), "spawn actions init");
        const int error = ::posix_spawnattr_init(&attributes_);
        if (error != 0) {
            ::posix_spawn_file_actions_destroy(&actions_);
            checkError(error, "spawn attributes init");
        }
    }
    ~SpawnSetup() {
        ::posix_spawnattr_destroy(&attributes_);
        ::posix_spawn_file_actions_destroy(&actions_);
    }
};

class ChildProcess : private netlib::Noncopyable {
public:
    ChildProcess(const std::string& executable, std::vector<std::string> arguments) {
        Pipe output, errors;
        SpawnSetup setup;
        checkError(::posix_spawn_file_actions_addopen(&setup.actions_, STDIN_FILENO,
                   "/dev/null", O_RDONLY, 0), "spawn stdin");
        checkError(::posix_spawn_file_actions_adddup2(&setup.actions_, output.write_.get(),
                   STDOUT_FILENO), "spawn stdout");
        checkError(::posix_spawn_file_actions_adddup2(&setup.actions_, errors.write_.get(),
                   STDERR_FILENO), "spawn stderr");
        for (int fd : {output.read_.get(), output.write_.get(), errors.read_.get(), errors.write_.get()})
            checkError(::posix_spawn_file_actions_addclose(&setup.actions_, fd), "spawn close pipe");
        sigset_t empty{}, defaults{};
        ::sigemptyset(&empty);
        ::sigemptyset(&defaults);
        for (int signal : {SIGINT, SIGTERM, SIGPIPE})
            ::sigaddset(&defaults, signal);
        checkError(::posix_spawnattr_setsigmask(&setup.attributes_, &empty), "spawn signal mask");
        checkError(::posix_spawnattr_setsigdefault(&setup.attributes_, &defaults), "spawn signal defaults");
        checkError(::posix_spawnattr_setflags(&setup.attributes_,
                   POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF), "spawn flags");
        arguments.insert(arguments.begin(), executable);
        std::vector<char*> argv;
        for (auto& argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        pid_t child = -1;
        checkError(::posix_spawn(&child, executable.c_str(), &setup.actions_, &setup.attributes_,
                   argv.data(), environ), "posix_spawn echo_server");
        // 成功启动之后只移动 fd，不再进行可能抛异常的分配。
        pid_ = child;
        output_ = std::move(output.read_);
        errors_ = std::move(errors.read_);
    }
    ~ChildProcess() {
        cleanup();
        // 进程可能在首次 poll 前就退出，仍尽量保留其错误输出。
        try { if (output_.get() >= 0) drain(output_, stdout_, false); } catch (...) {}
        try { if (errors_.get() >= 0) drain(errors_, stderr_, true); } catch (...) {}
        if (std::uncaught_exceptions() > 0) {
            std::cerr << "child pid=" << pid_ << " " << statusText()
                      << "\nstdout:\n" << stdout_ << "\nstderr (tail):\n" << stderr_ << '\n';
        }
    }
    const std::string& output() const { return stdout_; }
    const std::string& errors() const { return stderr_; }

    // 每次等待 socket 时也消费输出，防止服务端被写满的日志管道阻塞。
    void poll(std::vector<pollfd>& sockets, Time until) {
        reap();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
        check(remaining.count() > 0, "deadline exceeded");
        std::vector<pollfd> entries{{output_.get(), POLLIN, 0}, {errors_.get(), POLLIN, 0}};
        entries.insert(entries.end(), sockets.begin(), sockets.end());
        const int result = ::poll(entries.data(), entries.size(),
                                 static_cast<int>(std::min<long long>(remaining.count(), 50)));
        if (result < 0) {
            if (errno == EINTR)
                return;
            checkError(errno, "poll child/socket");
        }
        if (entries[0].revents)
            drain(output_, stdout_, false);
        if (entries[1].revents)
            drain(errors_, stderr_, true);
        for (size_t i = 0; i < sockets.size(); ++i)
            sockets[i].revents = entries[i + 2].revents;
        reap();
    }
    void requireAlive() {
        reap();
        check(!reaped_, "echo_server exited early: " + statusText());
    }
    void checkThreadCount(int workers) {
        requireAlive();
        const std::string path = "/proc/" + std::to_string(pid_) + "/task";
        DIR* directory = ::opendir(path.c_str());
        if (!directory)
            checkError(errno, "open child task directory");
        int count = 0;
        errno = 0;
        while (const auto* entry = ::readdir(directory))
            if (entry->d_name[0] != '.')
                ++count;
        const int error = errno;
        ::closedir(directory);
        checkError(error, "read child task directory");
        check(count == workers + 1, "--threads did not configure the actual worker count");
    }
    uint16_t waitReady() {
        const Time until = deadlineAfter(3s);
        while (stdout_.find('\n') == std::string::npos) {
            requireAlive();
            std::vector<pollfd> none;
            poll(none, until);
        }
        const std::string line = stdout_.substr(0, stdout_.find('\n'));
        const std::string prefix = "READY 0.0.0.0:";
        check(line.compare(0, prefix.size(), prefix) == 0, "unexpected ready line: " + line);
        const std::string port = line.substr(prefix.size());
        int value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        check(parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
              value > 0 && value <= 65535, "invalid advertised port: " + port);
        requireAlive();
        return static_cast<uint16_t>(value);
    }
    void signal(int value) {
        requireAlive();
        if (::kill(pid_, value) < 0)
            checkError(errno, "signal echo_server");
    }
    void waitExit(int expected) {
        const Time until = deadlineAfter(3s);
        while (!reaped_ || output_.get() >= 0 || errors_.get() >= 0) {
            std::vector<pollfd> none;
            poll(none, until);
        }
        check(WIFEXITED(status_) && WEXITSTATUS(status_) == expected,
              "unexpected child exit: " + statusText() + ", expected " + std::to_string(expected));
    }
    void checkStopped() const {
        const auto end = stdout_.find('\n');
        check(end != std::string::npos && stdout_.substr(end + 1) == "STOPPED\n",
              "missing or unexpected STOPPED notification");
    }
private:
    void reap() {
        if (reaped_)
            return;
        pid_t result;
        do { result = ::waitpid(pid_, &status_, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            if (error == ECHILD)
                reaped_ = true; // 已无可等待的子进程，清理路径不能向该 PID 再发信号。
            checkError(error, "waitpid echo_server");
        }
        reaped_ = result == pid_;
    }
    static void drain(Fd& pipe, std::string& text, bool tail) {
        // 单次有界消费，日志持续产生时也给 socket 和截止时间检查留下机会。
        for (size_t consumed = 0; consumed < 64 * 1024;) {
            char bytes[4096];
            const ssize_t count = ::read(pipe.get(), bytes, sizeof bytes);
            if (count == 0) {
                pipe.reset();
                return;
            }
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                checkError(errno, "read child pipe");
            }
            consumed += static_cast<size_t>(count);
            text.append(bytes, static_cast<size_t>(count));
            if (tail && text.size() > 64 * 1024)
                text.erase(0, text.size() - 64 * 1024);
            if (!tail)
                check(text.size() <= 8192, "unexpectedly large stdout");
        }
    }
    std::string statusText() const {
        if (!reaped_)
            return "running";
        if (WIFEXITED(status_))
            return "exit=" + std::to_string(WEXITSTATUS(status_));
        if (WIFSIGNALED(status_))
            return "signal=" + std::to_string(WTERMSIG(status_));
        return "status=" + std::to_string(status_);
    }
    void cleanup() noexcept {
        try {
            reap();
            if (!reaped_) {
                ::kill(pid_, SIGTERM);
                const Time until = Clock::now() + 500ms;
                while (!reaped_ && Clock::now() < until) {
                    std::vector<pollfd> none;
                    poll(none, until);
                }
            }
        } catch (...) {
            // 清理路径仍继续强制回收，不能因日志/管道错误遗留子进程。
        }
        if (!reaped_) {
            ::kill(pid_, SIGKILL);
            pid_t result;
            do { result = ::waitpid(pid_, &status_, 0); } while (result < 0 && errno == EINTR);
            reaped_ = result == pid_;
        }
    }
    pid_t pid_ = -1;
    int status_ = 0;
    bool reaped_ = false;
    Fd output_, errors_;
    std::string stdout_, stderr_;
};

short waitSocket(ChildProcess& child, int fd, short events, Time until, bool allowExit = false) {
    for (;;) {
        if (!allowExit)
            child.requireAlive();
        std::vector<pollfd> sockets{{fd, events, 0}};
        child.poll(sockets, until);
        check(!(sockets[0].revents & POLLNVAL), "invalid client fd");
        if (sockets[0].revents)
            return sockets[0].revents;
    }
}
Fd connectClient(ChildProcess& child, uint16_t port) {
    Fd socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (socket.get() < 0)
        checkError(errno, "client socket");
    const int buffer = 65536;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer) < 0)
        checkError(errno, "client receive buffer");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = ::htons(port);
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) {
        check(errno == EINPROGRESS, "client connect failed: " + std::to_string(errno));
        waitSocket(child, socket.get(), POLLOUT, deadlineAfter(3s));
        int error = 0;
        socklen_t length = sizeof error;
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0)
            checkError(errno, "client SO_ERROR");
        checkError(error, "client connect");
    }
    return socket;
}

struct Exchange {
    int fd_; // 借用本次收发期间由调用方持有的 socket。
    std::string bytes_;
    bool halfClose_ = false, slowRead_ = false;
    size_t sent_ = 0, received_ = 0;
    bool finSent_ = false, eof_ = false;
    Time readAfter_{};
    bool complete() const {
        return sent_ == bytes_.size() && received_ == bytes_.size() && (!halfClose_ || eof_);
    }
};

void exchange(ChildProcess& child, std::vector<Exchange> requests) {
    const Time until = deadlineAfter(6s);
    while (std::any_of(requests.begin(), requests.end(), [](const auto& r) { return !r.complete(); })) {
        child.requireAlive();
        std::vector<pollfd> sockets;
        for (auto& request : requests) {
            if (request.halfClose_ && !request.finSent_ && request.sent_ == request.bytes_.size()) {
                if (::shutdown(request.fd_, SHUT_WR) < 0)
                    checkError(errno, "client half-close");
                request.finSent_ = true;
                request.readAfter_ = Clock::now() + (request.slowRead_ ? 100ms : 0ms);
            }
            short events = request.sent_ < request.bytes_.size() ? POLLOUT : 0;
            if ((!request.slowRead_ || (request.finSent_ && Clock::now() >= request.readAfter_)) &&
                (request.received_ < request.bytes_.size() || (request.halfClose_ && !request.eof_)))
                events |= POLLIN;
            // events=0 的 socket 仍可能持续报告 HUP，用 -1 排除已完成/暂停的项目。
            sockets.push_back({events ? request.fd_ : -1, events, 0});
        }
        child.poll(sockets, until);
        for (size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            const short events = sockets[i].revents;
            check(!(events & POLLNVAL), "invalid exchange socket");
            if ((events & POLLOUT) && request.sent_ < request.bytes_.size()) {
                const ssize_t count = ::send(request.fd_, request.bytes_.data() + request.sent_,
                                             request.bytes_.size() - request.sent_, MSG_NOSIGNAL);
                if (count > 0)
                    request.sent_ += static_cast<size_t>(count);
                else if (count == 0 || !retryIo())
                    throw std::runtime_error("client send failed: " + std::to_string(errno));
            }
            if (events & (POLLIN | POLLHUP | POLLERR)) {
                char bytes[16384];
                const ssize_t count = ::recv(request.fd_, bytes, sizeof bytes, 0);
                if (count == 0) {
                    check(request.halfClose_ && request.finSent_ && request.received_ == request.bytes_.size(),
                          "early EOF truncated echo");
                    request.eof_ = true;
                } else if (count > 0) {
                    const size_t size = static_cast<size_t>(count);
                    check(size <= request.bytes_.size() - request.received_, "echo contained extra bytes");
                    check(request.bytes_.compare(request.received_, size, bytes, size) == 0,
                          "echo bytes differ at offset " + std::to_string(request.received_));
                    request.received_ += size;
                } else if (!retryIo()) {
                    checkError(errno, "client recv");
                }
            }
        }
    }
}

void expectEof(ChildProcess& child, int fd) {
    const Time until = deadlineAfter(3s);
    for (;;) {
        waitSocket(child, fd, POLLIN, until, true);
        char byte;
        const ssize_t count = ::recv(fd, &byte, 1, 0);
        if (count == 0)
            return;
        if (count < 0 && retryIo())
            continue;
        check(false, "active connection did not close cleanly after stop");
    }
}

std::string pattern(size_t length) {
    std::string bytes(length, '\0');
    for (size_t i = 0; i < length; ++i)
        bytes[i] = static_cast<char>(i % 251);
    return bytes;
}

void testCommandLine(const std::string& executable) {
    currentCase = "command line";
    {
        ChildProcess child(executable, {"--help"});
        child.waitExit(0);
        check(child.output().find("Usage: echo_server") != std::string::npos &&
              child.output().find("READY") == std::string::npos && child.errors().empty(),
              "help did not exit without starting the server");
    }
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"--port"}, {"--port", "65536"}, {"--threads", "-1"}, {"--unknown"}}) {
        ChildProcess child(executable, arguments);
        child.waitExit(2);
        check(child.output().empty() && child.errors().find("Usage: echo_server") != std::string::npos,
              "invalid command line did not report an error without starting");
    }
    std::cout << "[PASS] command line\n";
}

void testServer(const std::string& executable, int workers, int stopSignal) {
    currentCase = "workers=" + std::to_string(workers) + " signal=" + std::to_string(stopSignal);
    ChildProcess child(executable, {"--port", "0", "--threads", std::to_string(workers)});
    const uint16_t port = child.waitReady();
    child.checkThreadCount(workers);
    if (stopSignal == SIGINT) {
        child.signal(SIGINT);
        child.waitExit(0);
        child.checkStopped();
    } else {
        auto active = connectClient(child, port);
        const std::string text = u8"难过，先人🙂\r\n";
        for (int round = 0; round < 10; ++round)
            exchange(child, {{active.get(), text}});
        for (const std::string& fragment : {std::string("split"), std::string("\0\xff", 2), text})
            exchange(child, {{active.get(), fragment}});
        exchange(child, {{active.get(), pattern(2 * 1024 * 1024)}});

        std::vector<Fd> clients;
        std::vector<Exchange> concurrent;
        for (int i = 0; i < 8; ++i) {
            clients.push_back(connectClient(child, port));
            concurrent.push_back({clients.back().get(), pattern(64 * 1024) + std::to_string(i)});
        }
        exchange(child, std::move(concurrent));
        {
            auto slow = connectClient(child, port);
            exchange(child, {{slow.get(), pattern(8 * 1024 * 1024), true, true}});
        }
        exchange(child, {{active.get(), "after half-close"}});
        child.signal(SIGTERM);
        child.waitExit(0);
        child.checkStopped();
        expectEof(child, active.get());
        for (const auto& client : clients)
            expectEof(child, client.get());
    }
    std::cout << "[PASS] " << currentCase << '\n';
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: test_echo_process /path/to/echo_server\n";
        return 2;
    }
    suiteDeadline = Clock::now() + 20s; // 留出析构清理余量，CTest 外层超时为 30 秒。
    try {
        testCommandLine(argv[1]);
        for (int workers : {0, 1, 3}) {
            testServer(argv[1], workers, SIGINT);
            testServer(argv[1], workers, SIGTERM);
        }
        std::cout << "test_echo_process: all passed\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << currentCase << ": " << error.what() << '\n';
        return 1;
    }
    return 0;
}
