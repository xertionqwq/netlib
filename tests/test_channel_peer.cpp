#include <memory>

#include "netlib/acceptor.h"
#include "netlib/tcpConnection.h"

// 两个源文件包含完整 Reactor 头文件，链接时验证 header-only 定义。
std::unique_ptr<netlib::Channel> makeChannelInOtherTranslationUnit(
    netlib::EventLoop* loop, int fd) {
    return std::make_unique<netlib::Channel>(loop, fd);
}
