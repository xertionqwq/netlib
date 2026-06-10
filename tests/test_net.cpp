#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <iostream>

#include "netlib/eventloop.h"
#include "netlib/channel.h"
#include "netlib/inetAddress.h"
#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

using namespace netlib;

void test_channel_read() {
    int fds[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);

    EventLoop loop;
    Channel ch(&loop, fds[0]);

    bool called = false;
    ch.setReadCallback([&]() {
        char buf[16];
        ::recv(fds[0], buf, sizeof(buf), 0);
        called = true;
        loop.quit();
    });
    ch.enableReading();

    ::write(fds[1], "x", 1);
    loop.loop();  // 事件到达 → 回调 → quit → loop 返回

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

    ::close(fds[0]);
    ::close(fds[1]);
    assert(called && "write callback should have fired");
}

// 测试收数据：socketpair 客户端写 → TcpConnection 读 → messageCallback
void test_tcp_connection_read() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 8000);

    bool called = false;
    conn->setMessageCallback([&](const TcpConnection::TcpConnectionPtr&, Buffer& buf) {
        called = true;
        std::string data = buf.retrieveAsString(buf.readableBytes());
        assert(data == "hello");
        loop.quit();
    });

    conn->connectEstablished();
    ::write(fds[1], "hello", 5);
    loop.loop();

    ::close(fds[1]);
    assert(called);
}

// 测试发数据：TcpConnection send → socketpair 客户端 read
void test_tcp_connection_write() {
    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(&loop, fds[0], 8000);

    conn->connectEstablished();
    conn->send("world");
    // sendInLoop 已经在当前线程直接调了 ::write，数据已发

    char buf[16] = {};
    ssize_t n = ::recv(fds[1], buf, sizeof(buf) - 1, 0);
    assert(n == 5);
    assert(std::string(buf) == "world");

    ::close(fds[1]);
}

int main() {
    test_channel_read();
    std::cout << "[1/4] test_channel_read passed" << std::endl;

    test_channel_write();
    std::cout << "[2/4] test_channel_write passed" << std::endl;

    test_tcp_connection_read();
    std::cout << "[3/4] test_tcp_read passed" << std::endl;

    test_tcp_connection_write();
    std::cout << "[4/4] test_tcp_write passed" << std::endl;

    std::cout << "test_net: all passed" << std::endl;
    return 0;
}
