#ifndef NETLIB_DEFAULTPOLLER_H
#define NETLIB_DEFAULTPOLLER_H

#include <cstdlib>
#include <stdexcept>

#include <netlib/poller.h>
#include <netlib/epollpoller.h>

namespace netlib {

// 依据环境变量选择 IO 复用的方式
inline Poller *Poller::newDefaultPoller(EventLoop *loop) {
    if (::getenv("MUDUO_USE_POLL")) {
        throw std::logic_error("Poll backend is not implemented");
    } else {
        return new EPollPoller(loop); // 生成 EPoll 实例
    }
}
} // netlib

#endif // NETLIB_DEFAULTPOLLER_H
