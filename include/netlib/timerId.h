#ifndef NETLIB_TIMERID_H
#define NETLIB_TIMERID_H

#include <cstdint>

#include "netlib/noncopyable.h"

namespace netlib {

class Timer;

// 不透明标识, 用于取消定时器
class TimerId {
public:
    TimerId()
    : timer_(nullptr)
    , sequence_(0)
    { }

    TimerId(Timer *timer, int64_t seq)
    : timer_(timer)
    , sequence_(seq)
    { }

    friend class TimerQueue;

private:
    Timer *timer_;      // 借用 timer 指针
    int64_t sequence_;  // timer 标识
};
}

#endif // NETLIB_TIMERID_H