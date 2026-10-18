#include "netlib/socket.h"

#include <memory>

std::unique_ptr<netlib::Socket> makeSocketInOtherTranslationUnit(int fd) {
    return std::make_unique<netlib::Socket>(fd);
}

int acceptInOtherTranslationUnit(netlib::Socket& socket, netlib::InetAddress* peeraddr) {
    return socket.accept(peeraddr);
}
