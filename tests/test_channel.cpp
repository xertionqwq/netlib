#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

using namespace netlib;

std::unique_ptr<Channel> makeChannelInOtherTranslationUnit(EventLoop* loop, int fd);

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_channel failed: " << message << '\n';
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
        close(0);
        close(1);
    }
    int fd(int index) const { return fds_[index]; }
    int release(int index) {
        const int result = fds_[index];
        fds_[index] = -1;
        return result;
    }
    void close(int index) {
        if (fds_[index] >= 0)
            ::close(fds_[index]);
        fds_[index] = -1;
    }
private:
    int fds_[2]{-1, -1};
};

static void test_registration_lifecycle() {
    SocketPair data, wakeup;
    EventLoop loop;
    auto channel = makeChannelInOtherTranslationUnit(&loop, data.fd(0));
    Channel wakeupChannel(&loop, wakeup.fd(0));
    channel->disableAll();
    channel->remove();
    check(!loop.hasChannel(channel.get()), "unused channel was added to the loop");

    channel->enableReading();
    channel->enableWriting();
    channel->disableWriting();
    check(loop.hasChannel(channel.get()) && channel->index() == 1, "ADD/MOD state");
    channel->disableAll();
    channel->disableAll();
    check(loop.hasChannel(channel.get()) && channel->index() == 2,
          "disableAll must retain management without registering with epoll");

    int stage = 0;
    channel->setReadCallback([&](Timestamp receiveTime) {
        check(stage == 1, "disabled channel was dispatched before re-enabling");
        char byte;
        check(::read(data.fd(0), &byte, 1) == 1 && byte == 'd', "data read failed");
        check(receiveTime.toString().size() == 8, "receive timestamp missing");
        stage = 2;
        loop.quit();
    });
    wakeupChannel.setReadCallback([&](Timestamp) {
        char byte;
        check(::read(wakeup.fd(0), &byte, 1) == 1, "wakeup read failed");
        stage = 1;
        channel->enableReading();
    });
    wakeupChannel.enableReading();
    check(::write(data.fd(1), "d", 1) == 1, "data write failed");
    check(::write(wakeup.fd(1), "w", 1) == 1, "wakeup write failed");
    loop.loop();
    check(stage == 2, "re-enabled channel was not dispatched");

    channel->disableAll();
    channel->remove();
    channel->remove();
    check(!loop.hasChannel(channel.get()) && channel->index() == -1, "remove state");
    channel->enableReading();
    check(loop.hasChannel(channel.get()), "removed channel could not be re-added");
    channel->disableAll();
    channel->remove();
    wakeupChannel.disableAll();
    wakeupChannel.remove();
}

static void test_registration_failure() {
    EventLoop loop;
    Channel invalid(&loop, -1);
    bool failed = false;
    try {
        invalid.enableReading();
    } catch (const std::system_error& error) {
        failed = error.code().value() == EBADF;
    }
    check(failed, "invalid fd registration did not report EBADF");
    check(!loop.hasChannel(&invalid) && invalid.index() == -1,
          "failed registration retained state");
    invalid.disableAll();
    invalid.remove();

    SocketPair pair;
    Channel original(&loop, pair.fd(0)), duplicate(&loop, pair.fd(0));
    original.enableReading();
    failed = false;
    try {
        duplicate.enableReading();
    } catch (const std::logic_error&) {
        failed = true;
    }
    check(failed && loop.hasChannel(&original) && !loop.hasChannel(&duplicate),
          "duplicate registration damaged the original mapping");
    duplicate.disableAll();
    duplicate.remove();
    original.disableAll();
    original.remove();
}

static void test_hup_preserves_data() {
    SocketPair pair;
    EventLoop loop;
    auto connection = std::make_shared<TcpConnection>(&loop, pair.release(0), 8000);
    std::string received;
    int closed = 0;
    connection->setMessageCallback([&](const auto&, Buffer& buffer) {
        received += buffer.retrieveAsString(buffer.readableBytes());
    });
    connection->setCloseCallback([&](const auto&) {
        ++closed;
        loop.quit();
    });
    connection->connectEstablished();
    const std::string payload(4096, 'x');
    check(::write(pair.fd(1), payload.data(), payload.size())
          == static_cast<ssize_t>(payload.size()), "payload write failed");
    pair.close(1);
    loop.loop();
    check(received == payload && closed == 1, "HUP discarded data or closed twice");
    connection->connectDestroyed();
    connection->connectDestroyed();
}

static void test_half_close_and_fd_reuse() {
    SocketPair pair;
    EventLoop loop;
    const int originalFd = pair.fd(0);
    auto connection = std::make_shared<TcpConnection>(&loop, pair.release(0), 8000);
    std::string received;
    int closed = 0;
    connection->setMessageCallback([&](const auto& conn, Buffer& buffer) {
        received += buffer.retrieveAsString(buffer.readableBytes());
        conn->send("reply");
    });
    connection->setCloseCallback([&](const auto&) {
        ++closed;
        loop.quit();
    });
    connection->connectEstablished();
    check(::write(pair.fd(1), "last", 4) == 4, "last data write failed");
    check(::shutdown(pair.fd(1), SHUT_WR) == 0, "peer half-close failed");
    loop.loop();
    char reply[5];
    check(::read(pair.fd(1), reply, sizeof reply) == 5
          && std::string(reply, sizeof reply) == "reply", "half-close prevented reply");
    check(received == "last" && closed == 1, "RDHUP discarded data or closed twice");

    const int reusedFd = ::dup(pair.fd(1));
    check(reusedFd == originalFd, "closed fd was not reused");
    Channel replacement(&loop, reusedFd);
    replacement.enableReading();
    connection.reset();
    check(::fcntl(reusedFd, F_GETFD) >= 0 && loop.hasChannel(&replacement),
          "old connection closed a reused fd or removed its mapping");
    replacement.disableAll();
    replacement.remove();
    ::close(reusedFd);
}

static void test_terminal_callbacks() {
    SocketPair pair;
    EventLoop loop;
    Channel channel(&loop, pair.fd(0));
    std::string calls;
    channel.setCloseCallback([&]() { calls += 'C'; channel.disableAll(); });
    channel.setErrorCallback([&]() { calls += 'E'; channel.disableAll(); });
    channel.setReadCallback([&](Timestamp) { calls += 'R'; channel.disableAll(); });
    channel.setWriteCallback([&]() { calls += 'W'; });

    channel.enableReading();
    channel.enableWriting();
    channel.set_revents(EPOLLERR | EPOLLIN | EPOLLOUT);
    channel.handleEvent(Timestamp{});
    check(calls == "E", "terminal error continued to read/write callbacks");

    calls.clear();
    channel.enableReading();
    channel.enableWriting();
    channel.set_revents(EPOLLIN | EPOLLOUT);
    channel.handleEvent(Timestamp{});
    check(calls == "R", "terminal read continued to write callback");

    calls.clear();
    channel.enableWriting();
    channel.set_revents(EPOLLHUP | EPOLLERR | EPOLLOUT);
    channel.handleEvent(Timestamp{});
    check(calls == "C", "terminal HUP continued to other callbacks");

    calls.clear();
    channel.enableReading();
    channel.set_revents(EPOLLRDHUP);
    channel.handleEvent(Timestamp{});
    check(calls == "R", "RDHUP should invoke the read callback");
    channel.remove();
}

static void test_acceptor_cleanup() {
    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));
}

int main() {
    Logger::setLevel(LogLevel::WARN);
    test_registration_lifecycle();
    test_registration_failure();
    test_hup_preserves_data();
    test_half_close_and_fd_reuse();
    test_terminal_callbacks();
    test_acceptor_cleanup();
    std::cout << "test_channel: all passed\n";
}
