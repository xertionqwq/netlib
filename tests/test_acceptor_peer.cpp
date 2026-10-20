#include "netlib/acceptor.h"

#include <memory>

std::unique_ptr<netlib::Acceptor> makeAcceptorInOtherTranslationUnit(
    netlib::EventLoop* loop, const netlib::InetAddress& address, bool reuseport) {
    return std::make_unique<netlib::Acceptor>(loop, address, reuseport);
}

void listenInOtherTranslationUnit(netlib::Acceptor& acceptor) {
    acceptor.listen();
}
