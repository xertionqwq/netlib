#include <iostream>
#include <string>
#include "netlib/eventloop.h"
#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

// 简易 echo server：收到什么就回什么
// nc 127.0.0.1 8888 → 输入 hello → 返回 hello

int main() {
    std::vector<std::shared_ptr<netlib::TcpConnection>> connections;
    netlib::EventLoop loop;
    netlib::InetAddress addr(8888);
    netlib::Acceptor acceptor(&loop, addr);

    // 这里是设置回调, 实际没有任何连接建立
    acceptor.setNewConnectionCallback([&](int connfd, const netlib::InetAddress&) {
        // 创建并配置 TcpConnection
        auto conn = std::make_shared<netlib::TcpConnection>(&loop, connfd, 8888);
        connections.push_back(conn);  // 保住连接不被析构

        // 设置发送消息的回调函数
        // 我们将函数设为收到什么就发回什么
        conn->setMessageCallback([](const netlib::TcpConnection::TcpConnectionPtr& c,
                                     netlib::Buffer& buf) {
            std::string msg = buf.retrieveAsString(buf.readableBytes());
            if (!msg.empty() && msg.back() == '\n') msg.pop_back();
            if (!msg.empty() && msg.back() == '\r') msg.pop_back();
            c->send(msg + "\n");  // echo back
        });

        conn->connectEstablished();
    });

    acceptor.listen();
    std::cout << "Echo server listening on port 8888" << std::endl;
    loop.loop(); // 事件循环, 等待 fd 就绪
    return 0;
}
