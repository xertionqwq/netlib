#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include "netlib/eventloop.h"
#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

// 简易 echo server：收到什么就回什么
// nc 127.0.0.1 8888 → 输入 hello → 返回 hello

int main() {
    netlib::EventLoop loop;
    std::vector<netlib::TcpConnectionPtr> connections;
    netlib::InetAddress addr(8888);
    netlib::Acceptor acceptor(&loop, addr);

    // 这里是设置回调, 实际没有任何连接建立
    acceptor.setNewConnectionCallback([&](int connfd, const netlib::InetAddress& peerAddr) {
        sockaddr_in local{};
        socklen_t length = sizeof local;
        if (::getsockname(connfd, reinterpret_cast<sockaddr*>(&local), &length) < 0) {
            const int savedErrno = errno;
            ::close(connfd);
            NETLIB_LOG_ERROR << "getsockname fd=" << connfd << " errno=" << savedErrno;
            return;
        }
        // 创建并配置 TcpConnection
        netlib::InetAddress localAddr(0);
        localAddr.setSockAddrInet(local);
        auto conn = std::make_shared<netlib::TcpConnection>(
            &loop, peerAddr.toIpPort(), connfd, localAddr, peerAddr);
        connections.push_back(conn);  // 保住连接不被析构

        // 设置发送消息的回调函数
        // 我们将函数设为收到什么就发回什么
        conn->setMessageCallback([](const netlib::TcpConnectionPtr& c,
                                     netlib::Buffer* buf, netlib::Timestamp) {
            std::string msg = buf->retrieveAllAsString();
            if (!msg.empty() && msg.back() == '\n') msg.pop_back();
            if (!msg.empty() && msg.back() == '\r') msg.pop_back();
            c->send(msg + "\n");  // echo back
        });
        conn->setCloseCallback([&](const netlib::TcpConnectionPtr& closed) {
            loop.queueInLoop([&, closed] {
                closed->connectDestroyed();
                connections.erase(std::remove(connections.begin(), connections.end(), closed),
                                  connections.end());
            });
        });

        conn->connectEstablished();
    });

    acceptor.listen();
    std::cout << "Echo server listening on port 8888" << std::endl;
    loop.loop(); // 事件循环, 等待 fd 就绪
    for (const auto& conn : connections)
        conn->connectDestroyed();
    return 0;
}
