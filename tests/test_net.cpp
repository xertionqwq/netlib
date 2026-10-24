#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "netlib/eventloop.h"
#include "netlib/channel.h"
#include "netlib/inetAddress.h"
#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

using namespace netlib;

void test_inet_address() {
    InetAddress listenAddr(8080);
    assert(listenAddr.ip() == "0.0.0.0");
    assert(listenAddr.port() == 8080);
    assert(listenAddr.toIpPort() == "0.0.0.0:8080");
    assert(listenAddr.addr().sin_family == AF_INET);
    assert(listenAddr.size() == sizeof(sockaddr_in));

    InetAddress loopback("127.0.0.1", 65535);
    assert(loopback.ip() == "127.0.0.1");
    assert(loopback.port() == 65535);
    assert(loopback.toIpPort() == "127.0.0.1:65535");

    InetAddress broadcast("255.255.255.255", 0);
    assert(broadcast.ip() == "255.255.255.255");
    assert(broadcast.toIpPort() == "255.255.255.255:0");
}

void test_inet_address_invalid_ip() {
    const char* invalidIps[] = {"", "localhost", "256.0.0.1", "127.0.0", "::1"};
    for (const char* ip : invalidIps) {
        bool rejected = false;
        try {
            InetAddress addr(ip, 8080);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected && "invalid IPv4 address should be rejected");
    }
}

void test_channel_read() {
    int fds[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);

    EventLoop loop;
    Channel ch(&loop, fds[0]);

    bool called = false;
    ch.setReadCallback([&](Timestamp) {
        char buf[16];
        ::recv(fds[0], buf, sizeof(buf), 0);
        called = true;
        loop.quit();
    });
    ch.enableReading();

    ::write(fds[1], "x", 1);
    loop.loop();  // 事件到达 → 回调 → quit → loop 返回

    ch.disableAll();
    ch.remove();
    ::close(fds[0]);
    ::close(fds[1]);
    assert(called && "read callback should have fired");
}

void test_channel_write() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    EventLoop loop;
    Channel ch(&loop, fds[0]);
    ch.enableWriting();

    bool called = false;
    ch.setWriteCallback([&]() {
        called = true;
        ch.disableWriting();
        loop.quit();
    });
    ch.enableWriting();

    loop.loop();

    ch.disableAll();
    ch.remove();
    ::close(fds[0]);
    ::close(fds[1]);
    assert(called && "write callback should have fired");
}

// 测试收数据：socketpair 客户端写 → TcpConnection 读 → messageCallback
void test_tcp_connection_read() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(
        &loop, "net-read", fds[0], InetAddress(8000), InetAddress(0));

    bool called = false;
    conn->setMessageCallback([&](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
        called = true;
        std::string data = buf->retrieveAllAsString();
        assert(data == "hello");
        loop.quit();
    });

    conn->connectEstablished();
    ::write(fds[1], "hello", 5);
    loop.loop();

    conn->connectDestroyed();
    ::close(fds[1]);
    assert(called);
}

// 测试发数据：TcpConnection send → socketpair 客户端 read
void test_tcp_connection_write() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(
        &loop, "net-write", fds[0], InetAddress(8000), InetAddress(0));

    conn->connectEstablished();
    conn->send("world");
    // sendInLoop 已经在当前线程直接调用 ::send，数据已交给内核。

    char buf[16] = {};
    ssize_t n = ::recv(fds[1], buf, sizeof(buf) - 1, 0);
    assert(n == 5);
    assert(std::string(buf) == "world");

    conn->connectDestroyed();
    ::close(fds[1]);
}

int main() {
    test_inet_address();
    std::cout << "[1/6] test_inet_address passed" << std::endl;

    test_inet_address_invalid_ip();
    std::cout << "[2/6] test_inet_address_invalid_ip passed" << std::endl;

    test_channel_read();
    std::cout << "[3/6] test_channel_read passed" << std::endl;

    test_channel_write();
    std::cout << "[4/6] test_channel_write passed" << std::endl;

    test_tcp_connection_read();
    std::cout << "[5/6] test_tcp_read passed" << std::endl;

    test_tcp_connection_write();
    std::cout << "[6/6] test_tcp_write passed" << std::endl;

    std::cout << "test_net: all passed" << std::endl;
    return 0;
}
