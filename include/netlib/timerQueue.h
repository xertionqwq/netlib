#ifndef NETLIB_TIMERQUEUE_H
#define NETLIB_TIMERQUEUE_H

#include <set>
#include <map>
#include <vector>
#include <mutex>
#include <memory>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <unistd.h>
#include <sys/timerfd.h>

#include "netlib/timestamp.h"
#include "netlib/timer.h"
#include "netlib/timerId.h"
#include "netlib/Callbacks.h"
#include "netlib/log.h"
#include "netlib/channel.h"
#include "netlib/eventloop.h"
#include "netlib/noncopyable.h"

namespace netlib {

inline int createTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_FATAL << "timerfd_create failed: errno=" << savedErrno;
    }

    return timerfd;
}

inline struct timespec howMuchTimeFromNow(Timestamp when) {
    int64_t microseconds = when.microSecondsSinceEpoch() -
                           Timestamp::now().microSecondsSinceEpoch();
    if (microseconds < 100)
        microseconds = 100;

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(
        microseconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(
        (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

inline bool readTimerfd(int timerfd, Timestamp now) {
    uint64_t howmany = 0;
    ssize_t n;
    do {
        n = ::read(timerfd, &howmany, sizeof howmany);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        const int savedErrno = errno;
        if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
            NETLIB_LOG_ERROR << "read timerfd failed: errno=" << savedErrno;
        }
        return false;
    }
    if (n != static_cast<ssize_t>(sizeof howmany)) {
        NETLIB_LOG_ERROR << "read timerfd: expected 8 bytes, got " << n;
        return false;
    }
    NETLIB_LOG_INFO << "TimerQueue::handleRead() " << howmany
                    << " at " << now.toString();
    return true;
}

// timerfd 重新设置, 仅有两种情况可以发生
// 1. 新的定时任务入队, 并且发生的时间更早
// 2. 超时的任务出队执行, 堆顶的定时任务被设置为发生时间
inline void resetTimerfd(int timerfd, Timestamp expiration) {
    struct itimerspec newValue{};
    newValue.it_value = howMuchTimeFromNow(expiration);
    if (::timerfd_settime(timerfd, 0, &newValue, nullptr) < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_ERROR << "timerfd_settime failed: errno=" << savedErrno;
    }
}

/*
** 尽力而为的定时器队列
** 不保证任务准时进行
*/
class TimerQueue : private Noncopyable {
public:
    // 在所属 loop 线程构造并在事件分发之外析构，loop 必须存活更久。
    explicit TimerQueue(EventLoop *loop);
    ~TimerQueue();

    /*
    ** 支持跨线程调用，调用期间 TimerQueue 和 EventLoop 必须存活。
    ** 负责在给定的时间到达时执行回调函数
    ** 当 @c interval > 0.0 重复执行
    */
    TimerId addTimer(TimerCallback cb,
                     Timestamp when,
                     double interval);

    // 支持跨线程调用。取消已进入到期批次的任务时，只阻止它再次入队。
    void cancel(TimerId timerid);

private:
    // Value for map
    using TimerUPtr = std::unique_ptr<Timer>;
    // 单个定时器事件实体, 依靠时间点升序排序, Key for set
    using Entry = std::pair<Timestamp, Timer *>;
    // 按到期时间排序的索引，借用 Timer 指针
    using TimerList = std::set<Entry>;

    // Key for map/set
    using ActiveTimer = std::pair<Timer *, int64_t>;
    using ActiveTimerSet = std::set<ActiveTimer>;
    // 拥有定时器所有权
    using OwnerMap = std::map<ActiveTimer, TimerUPtr>;

    // 跨线程添加先保存在这里，直到 loop 接管或取消。
    struct PendingAdds {
        std::mutex mutex; // 该缓冲区的互斥锁
        OwnerMap timers; // 缓存区
    };

private:
    // 在定时器所在 loop 内执行任务更新
    void addTimerInLoop(TimerUPtr timer);
    void cancelInLoop(TimerId timerId);
    static TimerUPtr takePendingTimer(PendingAdds &pending, const ActiveTimer &timer);

    // 将任务加入到监听队列中
    bool insert(TimerUPtr timer);

    // 注册给 channel 的读回调
    // 当 poll 返回时, 也就是定时任务到期了, 某些定时器任务应该执行了
    void handleRead();

    // 移除所有过期定时任务
    std::vector<TimerUPtr> getExpired(Timestamp now);
    // 将需要重复执行的任务还回去, 同时重置下次timerfd时间
    void reset(std::vector<TimerUPtr> &expired, Timestamp now);

    // 两个任务集合/队列的数量必须严格一致
    void AssertSameSize() {
        NETLIB_ASSERT(timers_.size() == activeTimers_.size());
    }

private:
    EventLoop *loop_;
    int timerfd_;             // 拥有该 fd, 负责其生命
    std::unique_ptr<Channel> timerfdChannel_; // 注册到所在 loop 的 poller 中

    TimerList timers_; // 正在监听的事件实体

    // 用于完成并取消定时器任务
    OwnerMap activeTimers_;                 // 拥有已入队的定时任务
    std::atomic_bool callingExpiredTimers_; // 是否正在进行定时器任务
    ActiveTimerSet cancelingTimers_;        // 本批到期后不再入队的任务
    std::shared_ptr<PendingAdds> pendingAdds_; // 待办 lambda 通过弱引用检查队列是否存活
};

inline TimerQueue::TimerQueue(EventLoop *loop)
    : loop_(checkLoopInThread(loop))
    , timerfd_(-1)
    , callingExpiredTimers_(false)
    , pendingAdds_(std::make_shared<PendingAdds>())
{
    timerfd_ = createTimerfd();
    try {
        timerfdChannel_ = std::make_unique<Channel>(loop_, timerfd_);
        timerfdChannel_->setReadCallback([this](Timestamp) { handleRead(); });
        timerfdChannel_->enableReading();
    } catch (...) {
        ::close(timerfd_);
        throw;
    }
}

inline TimerQueue::~TimerQueue() {
    loop_->assertInLoopThread();
    pendingAdds_.reset();
    timerfdChannel_->disableAll();
    timerfdChannel_->remove();
    ::close(timerfd_);
}

inline TimerId TimerQueue::addTimer(TimerCallback cb,
                                   Timestamp when,
                                   double interval) {
    auto owner = std::make_unique<Timer>(std::move(cb), when, interval);
    TimerId id(owner.get(), owner->sequence());
    if (loop_->isInLoopTread()) {
        addTimerInLoop(std::move(owner));
        return id;
    }

    const ActiveTimer key(id.timer_, id.sequence_);
    {
        std::lock_guard<std::mutex> lock(pendingAdds_->mutex);
        const auto result = pendingAdds_->timers.try_emplace(key, std::move(owner));
        NETLIB_ASSERT(result.second);
    }

    // 弱引用检测待加入队列是否存活
    const std::weak_ptr<PendingAdds> pending = pendingAdds_;
    try {
        loop_->queueInLoop([this, pending, key] {
            if (auto state = pending.lock()) {
                if (auto timer = takePendingTimer(*state, key))
                    // 如果 addTimer 后立即 cancel, 则该 addInLoop 不会执行
                    addTimerInLoop(std::move(timer));
            }
        });
    } catch (...) {
        takePendingTimer(*pendingAdds_, key);
        throw;
    }
    return id;
}

inline void TimerQueue::cancel(TimerId timerId) {
    const std::weak_ptr<PendingAdds> pending = pendingAdds_;
    loop_->runInLoop([this, pending, timerId] {
        if (auto state = pending.lock())
            cancelInLoop(timerId);
    });
}

inline TimerQueue::TimerUPtr TimerQueue::takePendingTimer(PendingAdds &pending,
                                                         const ActiveTimer &timer) {
    std::lock_guard<std::mutex> lock(pending.mutex);
    auto node = pending.timers.extract(timer);
    return node ? std::move(node.mapped()) : nullptr;
}

inline void TimerQueue::addTimerInLoop(TimerUPtr timer) {
    loop_->assertInLoopThread();
    Timestamp when = timer->expiration();
    bool earliestChanged = insert(std::move(timer));

    if (earliestChanged) // 最早发生时间改变, 同步设置 timerfd
        resetTimerfd(timerfd_, when);
}

inline void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();
    AssertSameSize();

    ActiveTimer timer(timerId.timer_, timerId.sequence_);
    auto it = activeTimers_.find(timer);

    if (it != activeTimers_.end()) {
        // 先 timers_ 再 activeTimers_, 顺序不能乱
        // 否则 timers_ 会访问已析构的 timer
        size_t n = timers_.erase(Entry(it->second->expiration(), it->second.get()));
        NETLIB_ASSERT(n == 1);
        (void)n;
        activeTimers_.erase(it); // 智能指针, 销毁即析构, 避免手动 delete
    } else if (auto pendingTimer = takePendingTimer(*pendingAdds_, timer)) {
        // 如果 timers_ 找不到 timerId 说明还未插入, 此时可以移除共享区的 timer
        return;
    } else if (callingExpiredTimers_) { // 正在执行任务，记录本批取消请求
        cancelingTimers_.insert(timer);
    }

    AssertSameSize();
}

inline bool TimerQueue::insert(TimerUPtr timer) {
    loop_->assertInLoopThread();
    AssertSameSize();

    bool earliestChanged = false;
    Timestamp when = timer->expiration();
    auto it = timers_.begin();

    // 第一个定时任务/该任务更早发生
    if (it == timers_.end() || when < it->first)
        earliestChanged = true;

    // 插入 timers_
    const auto entry = timers_.emplace(when, timer.get());
    NETLIB_ASSERT(entry.second);
    // 插入 activeTimers_
    const ActiveTimer key(timer.get(), timer->sequence());
    try {
        const auto owner = activeTimers_.try_emplace(key, std::move(timer));
        NETLIB_ASSERT(owner.second);
    } catch (...) {
        timers_.erase(entry.first);
        throw;
    }

    // 两者数量始终一致
    AssertSameSize();
    return earliestChanged;
}

inline void TimerQueue::handleRead() {
    loop_->assertInLoopThread();

    Timestamp now(Timestamp::now());
    if (!readTimerfd(timerfd_, now)) // 读取失败, 本轮不执行
        return;

    auto expired = getExpired(now); // 得到待执行任务

    // 执行任务
    std::size_t completed = 0;
    callingExpiredTimers_ = true;
    cancelingTimers_.clear(); // 清空上一轮的暂存任务
    try {
        while (completed < expired.size()) {
            expired[completed]->run();
            ++completed;
        }
    } catch (...) {
        callingExpiredTimers_ = false;

        // 跳过下标为 completed 的失败任务，按原到期点放回后面未取消的任务。
        for (std::size_t i = completed + 1; i < expired.size(); ++i) {
            auto &timer = expired[i];
            if (cancelingTimers_.find({timer.get(), timer->sequence()}) == cancelingTimers_.end())
                insert(std::move(timer));
        }
        expired.resize(completed); // 只将已经完成的任务交给 reset()
        reset(expired, now);
        throw;
    }

    callingExpiredTimers_ = false;

    // 重设到期时间
    reset(expired, now);
}

inline std::vector<TimerQueue::TimerUPtr> TimerQueue::getExpired(Timestamp now) {
    AssertSameSize();

    std::vector<TimerUPtr> expired; // 待执行的任务队列, 对应 activeTimers_
    std::vector<Entry> entries; // 待取出的任务队列, 对应 timers_
    Entry sentry(now, reinterpret_cast<Timer *>(UINTPTR_MAX));
    // 找到已过期的任务边界
    auto end = timers_.lower_bound(sentry);
    NETLIB_ASSERT(end == timers_.end() || now < end->first);
    std::copy(timers_.begin(), end, back_inserter(entries));
    expired.reserve(entries.size()); // 提前分配内存
    timers_.erase(timers_.begin(), end);

    for (const auto &it : entries) {
        ActiveTimer key(it.second, it.second->sequence());
        auto owner = activeTimers_.find(key);
        NETLIB_ASSERT(owner != activeTimers_.end());
        expired.emplace_back(std::move(owner->second)); // 转移所有权
        size_t n = activeTimers_.erase(key);
        NETLIB_ASSERT(n == 1);
        (void)n;
    }

    AssertSameSize();
    return expired;
}

inline void TimerQueue::reset(std::vector<TimerQueue::TimerUPtr> &expired, Timestamp now) {
    Timestamp nextExpire;

    for (auto &it : expired) {
        // 未被取消, 并且需要重复的定时任务
        if (it->repeat()
            && cancelingTimers_.find({it.get(), it->sequence()}) == cancelingTimers_.end()) {
            it->restart(now);
            insert(std::move(it));
        }
    }

    if (!timers_.empty()) // 非空, 重新设置执行时间
        nextExpire = timers_.begin()->second->expiration();
    if (nextExpire.valid()) // 合法, 重置 timerfd
        resetTimerfd(timerfd_, nextExpire);
}

} // namespace netlib

#endif // NETLIB_TIMERQUEUE_H
