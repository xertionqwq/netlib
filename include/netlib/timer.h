#ifndef NETLIB_TIMER_H
#define NETLIB_TIMER_H

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <vector>
#include <set>
#include <cassert>

#include "netlib/log.h"

namespace netlib {

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> {
    /*
    ** 定时器类
    ** 包含 ms->多久后超时; next->绝对时间点, 表示需要触发的时刻; cb->超时后需要执行的回调函数
    ** 以 TimerManager 为友元类, 便于从 TimerSet 时间堆中删除/更新自己
    ** 拥有 Comparator 类, 用于比较 Timer 对象
    */

    friend class TimerManager;
private:
    // 最小堆比较函数, 用于比较两个 Timer 对象, 比较依据绝对超时时间
    struct Comparator {
        bool operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs) const {
            if (!lhs && !rhs) return false;
            if (!lhs) return true;   // null 排最前面
            if (!rhs) return false;
            return lhs->next_ < rhs->next_;  // 按绝对超时时间升序
        }
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
    /*
    ** 定时器管理器, 包装核心 TimeSet 时间堆, 管理定时器去留
    ** 只负责定时器在堆的生命, 不负责回调业务的执行
    ** 通过 add 添加定时器, list 取出定时器
    */

    friend class Timer;
public:
    using TimerSet = std::set<std::shared_ptr<Timer>, Timer::Comparator>;
    using FunCallback = std::function<void()>;

    TimerManager();
    virtual ~TimerManager(); // 析构设为虚函数, 防止 IOManager 继承时资源泄露

    // 添加 timer, ms->执行间隔, cb->定时器回调函数, recurring->是否循环定时器
    std::shared_ptr<Timer> addTimer(uint64_t ms, FunCallback cb, bool recurring = false);
    // 添加条件 timer, weakCond 为条件限制, 确保定时器对象生命
    std::shared_ptr<Timer> addConditionTimer
                        (uint64_t ms, FunCallback cb, std::weak_ptr<void> weakCond, bool recurring = false);

    uint64_t getNextTimer(); // 拿到堆中最近的定时器还有多久超时
    bool hasTimer();         // 堆中是否有定时器 timer
    // 取出所有超时定时器的回调函数
    void listExpiredCb(std::vector<FunCallback> &cbs);

protected:
    virtual void onTimerInsertAtFront() {};      // 最早的 timer 加入堆时, 调用该函数
    void addTimer(std::shared_ptr<Timer> timer); // 添加 timer

private:
    bool detectClockRollover(); // 检测系统是否发生了回滚

private:
    std::shared_mutex s_mtx_;
    TimerSet timers_;                // 时间堆, 存放所有 Timer 对象, 使用 Comparator 排序, 确保最早超时的 Timer 排在最前
    Timer::TimePoint previouseTime_; // 上次检查系统时间是否回退的绝对时间
    bool tickled_ = false;           // 确保 onTimerInsertedAtFront() 已经触发, 防止重复执行
};

//-----------------------Timer-------------------------
bool Timer::cancel() {
    // 独占锁
    std::unique_lock<std::shared_mutex> writeLock(manager_->s_mtx_);
    // 将回调函数设置为 nullptr
    if (cb_ == nullptr) {
        return false;
    } else {
        cb_ = nullptr;
    }

    // 找到要删除的定时器
    auto it = manager_->timers_.find(shared_from_this());
    if (it != manager_->timers_.end())
        manager_->timers_.erase(it);
    return true;
}
bool Timer::refresh() {
    // 独占锁
    std::unique_lock<std::shared_mutex> writeLock(manager_->s_mtx_);
    if (!cb_) // 没有回调函数
        return false;

    auto it = manager_->timers_.find(shared_from_this());
    if (it == manager_->timers_.end()) // 没找到
        return false;

    // 删除当前定时器并更新超时时间
    manager_->timers_.erase(it);
    next_ = std::chrono::system_clock::now() + std::chrono::milliseconds(ms_);
    manager_->timers_.insert(shared_from_this()); // 重新加入
    manager_->onTimerInsertAtFront();             // 唤醒
    return true;
}
bool Timer::reset(uint64_t ms, bool fromNow) {
    {
        std::shared_lock<std::shared_mutex> readLock(manager_->s_mtx_);
        if (ms == ms_ && !fromNow)
            return true; // 无需重置
    }

    {
        std::unique_lock<std::shared_mutex> writeLock(manager_->s_mtx_);
        if (!cb_) // 回调为空, 该定时器已经被取消或者未初始化
            return false; // 无法重置

        auto it = manager_->timers_.find(shared_from_this());
        if (it == manager_->timers_.end()) // 没有找到定时器
            return false;
        manager_->timers_.erase(it);
    }

    // 重新加入
    auto start = fromNow ? std::chrono::system_clock::now() : next_ - std::chrono::milliseconds(ms_);
    ms_ = ms;
    next_ = start + std::chrono::milliseconds(ms_);
    manager_->addTimer(shared_from_this()); // 注意, 此时在锁外, 使用 addTimer 更安全
    return true;
}

Timer::Timer(uint64_t ms, FunCallback cb, bool recurring, TimerManager *manager) :
ms_(ms), recurring_(recurring), cb_(std::move(cb)), manager_(manager) {
    auto now = std::chrono::system_clock::now();  // 记录当前时间
    next_ = now + std::chrono::milliseconds(ms_); // 下次超时时间
}

//--------------------TimerManager----------------------
TimerManager::TimerManager() {
    previouseTime_ = std::chrono::system_clock::now();
}
TimerManager::~TimerManager() {}

std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, FunCallback cb, bool recurring) {
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);
    return timer;
}
std::shared_ptr<Timer> TimerManager::addConditionTimer(
    uint64_t ms, FunCallback cb, std::weak_ptr<void> weakCond, bool recurring
) {
    return addTimer(ms, [=]()
                    {   // 确保对象仍然存在, 其中 lock 操作线程安全
                        std::shared_ptr<void> tmp = weakCond.lock();
                        if (tmp)
                            cb();
                    }, recurring);
}
void TimerManager::addTimer(std::shared_ptr<Timer> timer) {
    bool atFront = false;
    {
        std::unique_lock<std::shared_mutex> writeLock(s_mtx_);
        auto it = timers_.insert(timer).first;
        atFront = (it == timers_.begin()) && !tickled_; // 判断插入的定时器是否为最早超时的

        if (atFront)
            tickled_ = true; // 防止重复唤醒
    }
    if (atFront)
        onTimerInsertAtFront(); // 唤醒, 具体由 IOScheduler 执行
}

uint64_t TimerManager::getNextTimer() {
    std::shared_lock<std::shared_mutex> readLock(s_mtx_);
    tickled_ = false; // 重置
    if (timers_.empty())
        return ~0ull; // 返回最大值

    auto now = std::chrono::system_clock::now(); // 获取当前时间
    auto time = (*timers_.begin())->next_;       // 获取最小堆的第一个超时定时器并判断是否超时

    if (now >= time) {
        return 0; // 该 timer 已超时
    } else {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
        return static_cast<uint64_t>(duration.count()); // 返回时间差
    }
}
bool TimerManager::hasTimer() {
    std::shared_lock<std::shared_mutex> readLock(s_mtx_);
    return !timers_.empty();
}
void TimerManager::listExpiredCb(std::vector<FunCallback> &cbs) {
    auto now = std::chrono::system_clock::now();
    std::unique_lock<std::shared_mutex> writeLock(s_mtx_);
    bool rollover = detectClockRollover();

    while (!timers_.empty()) {
        auto time = (*timers_.begin())->next_;
        if (!rollover && now < time) // 系统没有发生回滚且没有超时的 timer
            break;
        std::shared_ptr<Timer> temp = *timers_.begin();
        timers_.erase(timers_.begin());
        cbs.push_back(temp->cb_);

        if (temp->recurring_) { // 处于循环态, 重新计算超时点, 并重新加入
            temp->next_ = now + std::chrono::milliseconds(temp->ms_);
            timers_.insert(temp);
            onTimerInsertAtFront(); // 唤醒
        } else { // 清理 cb
            temp->cb_ = nullptr;
        }
    }
}

bool TimerManager::detectClockRollover() {
    bool rollover = false;
    auto now = std::chrono::system_clock::now();
    if (now < (previouseTime_ - std::chrono::milliseconds(60 * 60 * 1000)))
        rollover = true;
    previouseTime_ = now;
    return rollover;
}

} // namespace netlib 


#endif // NETLIB_TIMER_H