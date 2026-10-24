#include "netlib/tcpConnection.h"

// 在另一翻译单元构造连接并调用关闭接口，检查 header-only 的定义和链接。
netlib::TcpConnectionPtr makeTcpConnectionInOtherTranslationUnit(netlib::EventLoop* loop, int fd) {
    return std::make_shared<netlib::TcpConnection>(
        loop, "tcpconnection-test", fd, netlib::InetAddress(8080),
        netlib::InetAddress("127.0.0.1", 9000));
}

void shutdownTcpConnectionInOtherTranslationUnit(const netlib::TcpConnectionPtr& conn) {
    conn->shutdown();
}

void forceCloseTcpConnectionInOtherTranslationUnit(const netlib::TcpConnectionPtr& conn) {
    conn->forceClose();
}
