#ifndef NETLIB_TIMER_H
#define NETLIB_TIMER_H

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <vector>
#include <set>
#include <cassert>

namespace netlib {

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> {
    friend class TimerManager;
private:
    // 最小堆比较函数, 用于比较两个 Timer 对象, 比较依据绝对超时时间
    struct Comparator {
        bool operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs);
    };

public:
    using FunCallback = std::function<void()>;
    using TimePoint = std::chrono::time_point<std::chrono::system_clock>;

    bool cancel();                           // 从时间堆中删除 timer
    bool refresh();                          // 刷新 timer 
    bool reset(uint64_t ms, bool fromNow);   // 重设 timer 超时时间

private:
    Timer(uint64_t ms, FunCallback cb, bool recurring, TimerManager *manager);

private:
    bool recurring_ = false;          // 是否循环
    uint64_t ms_ = 0;                 // 超时时间
    FunCallback cb_;                  // 超时触发的回调函数
    TimePoint next_;                  // 绝对超时时间, 该触发器下次触发的时间点
    TimerManager *manager_ = nullptr; // 管理 timer 的管理器
};

class TimerManager {
    friend class Timer;
public:
    using TimerSet = std::set<std::shared_ptr<Timer>, Timer::Comparator>;
    using FunCallback = std::function<void()>;

    TimerManager();
    virtual ~TimerManager();

    // 添加 timer, ms->执行间隔, cb->定时器回调函数, recurring->是否循环定时器
    std::shared_ptr<Timer> addTimer(uint64_t ms, FunCallback cb, bool recurring = false);
    // 添加条件 timer, weakCond 为条件限制
    std::shared_ptr<Timer> addConditionTimer
                        (uint64_t ms, FunCallback cb, std::weak_ptr<void> weakCond, bool recurring = false);

    uint64_t getNextTimer(); // 拿到堆中最近的超时时间
    bool hasTimer();         // 堆中是否右定时器 timer
    // 取出所有超时定时器的回调函数
    void listExpiredCb(std::vector<FunCallback> &cbs);

protected:
    virtual void onTimerInsertAtFront() {};      // 最早的 timer 加入堆时, 调用该函数
    void addTimer(std::shared_ptr<Timer> timer); // 添加 timer

private:
    bool detectClockRollover(); // 系统时间改变时调用该函数, 用于重置时间刻

private:
    std::shared_mutex s_mtx_;
    TimerSet timers_;                // 时间堆, 存放所有 Timer 对象, 使用 Comparator 排序, 确保最早超时的 Timer 排在最前
    Timer::TimePoint previouseTime_; // 上次检查系统时间是否回退的绝对时间
    bool tickled_ = false;           // 确保 onTimerInsertedAtFront() 已经触发, 防止重复执行
};

} // namespace netlib 


#endif // NETLIB_TIMER_H