#ifndef NETLIB_EVENTLOOP_H
#define NETLIB_EVENTLOOP_H

#include <thread>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cassert>
#include <memory>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

#include "netlib/poller.h"
#include "netlib/channel.h"
#include "netlib/noncopyable.h"
#include "netlib/currentThread.h"
#include "netlib/defaultPoller.h"

namespace netlib{

class EventLoop : private Noncopyable{

    /*
    ** 事件循环类, 主要包含两个大模块 Channel Poller
    ** Bind with C++17
    ** noncopyable
    */
public:
    using Functor = std::function<void()>;

    void assertInLoopThread() const {
        NETLIB_ASSERT(threadId_ == CurrentThread::tid());
    }
    bool isInLoopTread() const {
        return threadId_ == CurrentThread::tid();
    }

    explicit EventLoop(int timeWaitMs = 10000);
    ~EventLoop();

    // getter/setter
    int pollTimeoutMs() { return pollTimeoutMs_; }
    void setPollTimeoutMs(int ms) {
        assertInLoopThread();
        if (ms < -1)
            throw std::invalid_argument("invalid poll timeout");
        pollTimeoutMs_ = ms;
    }

    // 开启事件循环
    void loop();
    // 退出事件循环
    void quit() {
        quit_ = true;

        // 外部线程请求退出时，唤醒所属 loop 线程
        if (!isInLoopTread())
            wakeup();
    }

    Timestamp pollReturnTime() const { return pollReturnTime_; }

    // 在当前 loop 中执行回调
    void runInLoop(Functor);
    // 将回调放入队列中, 唤醒 loop 所在线程并执行回调
    void queueInLoop(Functor&&);

    // 唤醒 loop 所在线程
    void wakeup();

    // 连接 channel 与 poller 的桥梁, 用于更新 channel 相关信息
    void updateChannel(Channel* ch) { poller_->updateChannel(ch); }
    void removeChannel(Channel* ch) {
        assertInLoopThread();
        NETLIB_ASSERT(ch->ownerLoop() == this);
        // 本批其他活跃 Channel 仍可能被遍历，不允许提前移除并析构
        NETLIB_ASSERT(!eventHandling_ || currentActiveChannel_ == ch ||
                      std::find(activeChannels_.begin(), activeChannels_.end(), ch)
                          == activeChannels_.end());
        poller_->removeChannel(ch);
    }
    bool hasChannel(const Channel* ch) const {
        assertInLoopThread();
        return poller_->hasChannel(ch);
    }

private:
    void handleRead();                  // 消费当前 loop 的唤醒通知
    void doPendingFunctors();           // 执行回调

    void printActiveChannels() const;   // debug

private:
    using ChannelList = std::vector<Channel *>;

    const pid_t threadId_;              // 当前线程运行下的 id

    Timestamp pollReturnTime_;              // poller 返回发送时间的时间点
    std::unique_ptr<Poller> poller_;        // 封装的 epoll 对象

    ChannelList activeChannels_;            // 活跃的事件
    Channel *currentActiveChannel_;         // 进行相关断言操作
    int pollTimeoutMs_;                     // 超时时间, 默认为 10s

    int wakeupFd_;                          // 唤醒当前 loop 所属线程
    std::unique_ptr<Channel> wakeupChannel_;

    std::atomic_bool looping_;      // 是否正在循环 原子变量
    std::atomic_bool eventHandling_; // 是否正在处理事件
    std::atomic_bool quit_;         // 标志退出 loop 循环

    std::atomic_bool callingPendingFunctors_;   // 是否正在执行已取出的待办任务
    std::vector<Functor> pendingFunctors_;      // 等待执行的任务
    std::mutex mutex_;                          // 保护回调容器的线程安全
};

// 仅检查借用指针非空，允许在其他线程中验证目标 loop。
inline EventLoop* checkLoop(EventLoop* loop) {
    if (!loop)
        throw std::invalid_argument("EventLoop must not be null");
    return loop;
}

// 需要操作所属 loop 的资源时，同时检查当前线程。
inline EventLoop* checkLoopInThread(EventLoop* loop) {
    checkLoop(loop)->assertInLoopThread();
    return loop;
}

// 记录本线程的 EventLoop，每个线程至多有一个存活实例
inline thread_local EventLoop *loopPtr = nullptr;

// 创建 eventfd，供外部线程通知当前 loop
inline int createEventfd() {
    int evfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0)
        NETLIB_LOG_FATAL << "eventfd error: " << errno << '\n';
    return evfd;
}

inline EventLoop::EventLoop(int timeWaitMs)
    : threadId_(CurrentThread::tid()),
      pollReturnTime_(),
      poller_(Poller::newDefaultPoller(this)),
      currentActiveChannel_(nullptr),
      pollTimeoutMs_(timeWaitMs),
      wakeupFd_(-1),
      looping_(false),
      eventHandling_(false),
      quit_(false),
      callingPendingFunctors_(false)
{
    if (timeWaitMs < -1)
        throw std::invalid_argument("invalid poll timeout");
    NETLIB_LOG_DEBUG << "EventLoop created " << this
                     << " in thread " << threadId_ << '\n';
    if (loopPtr) { // 当前线程已经有 loop 了
        NETLIB_LOG_FATAL << "Another Event " << loopPtr
                         << " exists in this thread " << loopPtr->threadId_ << '\n';
    }
    wakeupFd_ = createEventfd();
    try {
        wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
        wakeupChannel_->setReadCallback([this](Timestamp) { handleRead(); });
        wakeupChannel_->enableReading();
    } catch (...) {
        ::close(wakeupFd_);
        wakeupFd_ = -1;
        throw;
    }
    // 全部初始化成功后再登记，构造失败不会留下悬空指针
    loopPtr = this;
}
inline EventLoop::~EventLoop() {
    assertInLoopThread();
    NETLIB_ASSERT(!looping_);
    // 管理 wakeup 资源
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    loopPtr = nullptr; // 清除当前 Loop 登记
    quit_ = true;
}

// 事件循环, 核心函数
inline void EventLoop::loop() {
    assertInLoopThread();
    NETLIB_ASSERT(!looping_);

    looping_ = true;

    try {
        NETLIB_LOG_INFO << "EventLoop " << this << " start looping" << '\n';
        while (!quit_) {
            activeChannels_.clear();
            pollReturnTime_ = poller_->poll(pollTimeoutMs_, activeChannels_);
            eventHandling_ = true;
            for (Channel *channel : activeChannels_) {
                currentActiveChannel_ = channel;
                currentActiveChannel_->handleEvent(pollReturnTime_);
            }
            currentActiveChannel_ = nullptr;
            eventHandling_ = false;
            doPendingFunctors();
        }
    } catch (...) {
        // 回调异常传回 loop 调用者，同时允许安全清理当前 EventLoop
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;
        callingPendingFunctors_ = false;
        looping_ = false;
        throw;
    }

    looping_ = false;
    NETLIB_LOG_INFO << "EventLoop " << this << " stop looping" << '\n';
}

inline void EventLoop::handleRead() {
    uint64_t one = 0;
    ssize_t n;
    do {
        n = ::read(wakeupFd_, &one, sizeof one);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN)
        return;
    if (n != static_cast<ssize_t>(sizeof one)) {
        NETLIB_LOG_ERROR << "EventLoop::handleRead() reads " << n
                         << " bytes instead of 8" << '\n';
    }
}

inline void EventLoop::runInLoop(Functor cb) {
    if (isInLoopTread()) // 当前 loop 线程中执行 cb
        cb();
    else // 在非当前 loop 中执行 cb, 需要唤醒 loop 所在线程
        queueInLoop(std::move(cb));
}

inline void EventLoop::queueInLoop(Functor &&cb) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
    }

    // 唤醒相应的, 需要执行上面回调操作的 loop 线程
    if (!isInLoopTread() || callingPendingFunctors_ || !looping_)
        wakeup();
}

// 用来唤醒 loop 所在的线程 向 wakeupfd_ 写一个数据
inline void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n;
    do {
        n = ::write(wakeupFd_, &one, sizeof one);
    } while (n < 0 && errno == EINTR);
    // 计数器已满时，本来就有未消费的通知，无需再次写入
    if (n < 0 && errno == EAGAIN)
        return;
    if (n != static_cast<ssize_t>(sizeof one)) {
        NETLIB_LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8" << '\n';
    }
}

// 在所属线程中执行取出的任务，执行期间不持有 mutex_；异常会中断当前批次
inline void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true; // 开始执行回调

    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    } // 退出锁时, 新的回调队列可以异步接收新的回调, 不影响局部回调队列的执行

    for (const Functor &functor : functors)
        functor();

    callingPendingFunctors_ = false; // 回调执行结束
}

inline void EventLoop::printActiveChannels() const {
    assertInLoopThread();
    for (const Channel *channel : activeChannels_)
        NETLIB_LOG_DEBUG << "active channel fd=" << channel->fd();
}

// Channel 的方法需要 EventLoop 完整定义
/*
** 当 channel 改变 fd 的 events 事件后
** 负责更改 poller 中对应 fd 的事件
*/
inline void Channel::update() {
    loop_->updateChannel(this);
}
// 在 channel 所属的 loop 中, 从 poller 将当前的 channel 移除
inline void Channel::remove() {
    assert(isNoneEvent()); // 确实无事可做了
    loop_->removeChannel(this);
}

inline void Poller::assertInLoopThread() {
    ownerLoop_->assertInLoopThread();
}
};

#endif // NETLIB_EVENTLOOP_H
