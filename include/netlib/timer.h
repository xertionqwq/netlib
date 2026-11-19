#ifndef NETLIB_TIMER_H
#define NETLIB_TIMER_H

#include <atomic>

#include "netlib/timestamp.h"
#include "netlib/Callbacks.h"
#include "netlib/noncopyable.h"
#include "netlib/log.h"

namespace netlib {

/*
** 内部事件时间类
** 支持到时执行
** 支持定时执行, 一旦设置便重复
*/
class Timer : Noncopyable {
public:
    Timer(TimerCallback cb, Timestamp when, double interval = 0.0)
    : cb_(std::move(cb))
    , expiration_(when)
    , interval_(interval)
    , repeat_(interval > 0.0)
    , sequence_(++s_numCreated_)
{ }

    void run() const { cb_(); }
    void restart(Timestamp now) {
        if (repeat_)
            expiration_ = addTime(now, interval_);
        else
            expiration_ = Timestamp::invalid();
    }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    static int64_t numCreated() { return s_numCreated_; }

private:
    const TimerCallback cb_;                  // 需要执行的回调
    Timestamp expiration_;                    // 到期点, 绝对时间
    const double interval_;                   // 间隔期, 相对时间
    const bool repeat_;                       // 是否需要重复执行
    const int64_t sequence_;                  // 当前 Timer 的序列号
    static std::atomic_int64_t s_numCreated_; // 已创建的 Timer 个数
};

inline std::atomic_int64_t Timer::s_numCreated_{0};

} // namespace netlib

#endif // NETLIB_TIMER_H