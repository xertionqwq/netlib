#ifndef NEILIB_DEFAULTPOLLER_H
#define NEILIB_DEFAULTPOLLER_H

#include <stdlib.h>

#include <netlib/poller.h>

namespace netlib {

// 依据环境变量选择 IO 复用的方式
Poller *Poller::newDefaultPoller(EventLoop *Loop) {
    if (::getenv("MUDUO_USE_POLL")) {
        return nullptr; // 生成 Poll 实例
    } else {
        return nullptr; // 生成 EPoll 实例
    }
}
} // netlib

#endif // NEILIB_DEFAULTPOLLER_H