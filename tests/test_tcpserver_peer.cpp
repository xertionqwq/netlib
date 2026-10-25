#include "netlib/tcpServer.h"

std::unique_ptr<netlib::TcpServer> makeServerInOtherTranslationUnit(
    netlib::EventLoop* loop, netlib::TcpServer::Option option) {
    return std::make_unique<netlib::TcpServer>(
        loop, netlib::InetAddress("127.0.0.1", 0), "server-test", option);
}

void startServerInOtherTranslationUnit(netlib::TcpServer& server) {
    server.start();
}
