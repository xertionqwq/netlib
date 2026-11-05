#ifndef NETLIB_ECHOSERVER_H
#define NETLIB_ECHOSERVER_H

#include <charconv>
#include <climits>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <system_error>
#include <sys/signalfd.h>

#include "netlib/tcpServer.h"

// 示例专用支持代码，不属于安装的公共头文件。
namespace netlib::example {

struct EchoOptions {
    uint16_t port = 12300;
    int threads = 3;
    bool help = false;
};

inline EchoOptions parseEchoOptions(int argc, const char* const argv[]) {
    EchoOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--help") {
            options.help = true;
            continue;
        }
        if (option != "--port" && option != "--threads")
            throw std::invalid_argument("unknown option: " + option);
        if (++i == argc)
            throw std::invalid_argument("missing value for " + option);
        const std::string text(argv[i]);
        int value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        const int maximum = option == "--port" ? 65535 : INT_MAX;
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            value < 0 || value > maximum)
            throw std::invalid_argument("invalid value for " + option + ": " + text);
        if (option == "--port")
            options.port = static_cast<uint16_t>(value);
        else
            options.threads = value;
    }
    return options;
}

inline void printEchoUsage(std::ostream& output) {
    output << "Usage: echo_server [--port 0..65535] [--threads N] [--help]\n"
           << "Defaults: port 12300, threads 3; port 0 selects an ephemeral port.\n"
           << "SIGINT/SIGTERM stops the event loop and cleans up connections.\n";
}

// 必须在创建工作线程之前阻塞信号，确保工作线程继承屏蔽字。
class StopSignalMask : private Noncopyable {
public:
    StopSignalMask() {
        ::sigemptyset(&mask_);
        ::sigaddset(&mask_, SIGINT);
        ::sigaddset(&mask_, SIGTERM);
        const int error = ::pthread_sigmask(SIG_BLOCK, &mask_, &previous_);
        if (error != 0)
            throw std::system_error(error, std::generic_category(), "pthread_sigmask");
    }
    ~StopSignalMask() {
        const int error = ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        if (error != 0) {
            NETLIB_LOG_ERROR << "restore signal mask errno=" << error;
        }
    }
    const sigset_t& mask() const { return mask_; }
private:
    sigset_t mask_{}, previous_{};
};

// 将停止请求作为普通读事件处理，不在异步信号处理器中操作 EventLoop。
// 在所属线程构造、析构；必须比全部工作线程存活更久，在事件分发之外析构。
class SignalStop : private Noncopyable {
public:
    explicit SignalStop(EventLoop* loop)
        : loop_(checkLoopInThread(loop)), fd_(createFd(mask_.mask())), channel_(loop_, fd_) {
        try {
            channel_.setReadCallback([this](Timestamp) {
                for (;;) {
                    signalfd_siginfo signal{};
                    ssize_t count;
                    do {
                        count = ::read(fd_, &signal, sizeof signal);
                    } while (count < 0 && errno == EINTR);
                    if (count == static_cast<ssize_t>(sizeof signal)) {
                        loop_->quit();
                        continue; // 消费同批待处理的停止信号，再返回事件循环。
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        return;
                    const int error = count < 0 ? errno : EIO;
                    throw std::system_error(error, std::generic_category(), "read signalfd");
                }
            });
            channel_.enableReading();
        } catch (...) {
            channel_.disableAll();
            channel_.remove();
            ::close(fd_);
            throw;
        }
    }
    ~SignalStop() {
        loop_->assertInLoopThread();
        channel_.disableAll();
        channel_.remove();
        ::close(fd_);
    }
private:
    static int createFd(const sigset_t& mask) {
        const int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(), "signalfd");
        }
        return fd;
    }
    EventLoop* loop_;
    StopSignalMask mask_;
    const int fd_;
    Channel channel_;
};

class EchoServer {
public:
    EchoServer(EventLoop* loop, const InetAddress& addr, const std::string& name, int threads)
        : server_(loop, addr, name) {
        server_.setConnectionCallback([](const TcpConnectionPtr& conn) {
            NETLIB_LOG_INFO << "Connection " << (conn->connected() ? "UP : " : "DOWN : ")
                            << conn->peerAddress().toIpPort();
        });
        server_.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
            const size_t bytes = buf->readableBytes();
            conn->send(buf);
            NETLIB_LOG_DEBUG << "echo " << bytes << " bytes at time: " << time.toString();
        });
        server_.setThreadNum(threads);
    }
    void start() { server_.start(); }
    InetAddress listenAddress() const { return server_.listenAddress(); }
    void printReady(std::ostream& output) const {
        output << "READY " << listenAddress().toIpPort() << std::endl;
    }
private:
    TcpServer server_;
};

} // namespace netlib::example

#endif // NETLIB_ECHOSERVER_H
