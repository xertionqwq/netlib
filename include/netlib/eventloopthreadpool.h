#ifndef NETLIB_EVENTLOOPTHREADPOOL_H
#define NETLIB_EVENTLOOPTHREADPOOL_H

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "netlib/eventloopthread.h"
#include "netlib/log.h"
#include "netlib/noncopyable.h"

namespace netlib {

// 管理多个 EventLoopThread，为新连接选择所属的 EventLoop。
// 构造、配置、启动、查询与析构均在 baseLoop 所属线程串行执行。
class EventLoopThreadPool : private Noncopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    // baseLoop 是借用指针，必须比线程池存活更久；构造时不启动工作线程。
    explicit EventLoopThreadPool(EventLoop* baseLoop, const std::string &name);
    // 回收工作线程，不负责停止或销毁 baseLoop。
    ~EventLoopThreadPool();

    // 仅在启动前设置，线程数不能为负；0 表示只使用 baseLoop。
    void setThreadNum(int numThreads) {
        baseLoop_->assertInLoopThread();
        if (startAttempted_)
            throw std::logic_error("Cannot configure EventLoopThreadPool after startup was attempted");
        if (numThreads < 0)
            throw std::invalid_argument("EventLoopThreadPool thread count must not be negative");
        numThreads_ = numThreads;
    }

    // 仅允许启动一次；等待各工作循环初始化完成，可选回调在各工作线程执行。
    // 工作线程数为 0 时，在 baseLoop 所属线程执行初始化回调。
    // 启动过程抛出异常时清理已创建线程，不支持重试或获取循环。
    void start(ThreadInitCallback cb = ThreadInitCallback());

    // start() 成功返回后，轮询工作循环；没有工作线程时返回 baseLoop。
    EventLoop* getNextLoop();
    // 启动后返回全部工作循环；没有工作线程时返回仅含 baseLoop 的容器。
    // 两个接口返回的 loop 均为借用指针，仅在对应 EventLoop 存活时有效。
    // 使用期间不得让工作循环自行退出；线程池销毁后不能继续使用这些指针。
    std::vector<EventLoop*> getAllLoops() const;

    bool started() const {
        baseLoop_->assertInLoopThread();
        return started_;
    }
    const std::string &name() const {
        baseLoop_->assertInLoopThread();
        return name_;
    }

private:
    EventLoop* baseLoop_;     // 外部拥有的主循环，不负责释放
    std::string name_;        // 线程池名称，用于生成工作线程名称
    bool startAttempted_;     // 是否已尝试启动，启动中和失败后均不允许重试
    bool started_;            // 是否已完成启动
    int numThreads_;          // 额外创建的工作线程数量，不包含 baseLoop
    std::size_t next_;        // 下一次轮询 loops_ 使用的下标

    std::vector<std::unique_ptr<EventLoopThread>> threads_; // 独占工作线程包装器
    std::vector<EventLoop*> loops_; // 借用工作线程栈上的循环，不包含 baseLoop
};


inline EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string &name)
    : baseLoop_(baseLoop)
    , name_(name)
    , startAttempted_(false)
    , started_(false)
    , numThreads_(0)
    , next_(0) {
    if (!baseLoop_)
        throw std::invalid_argument("EventLoopThreadPool baseLoop must not be null");
    baseLoop_->assertInLoopThread();
}

inline EventLoopThreadPool::~EventLoopThreadPool() {
    baseLoop_->assertInLoopThread();
    // loops_ 仅保存借用指针；threads_ 析构时由 EventLoopThread 请求退出，
    // 等待工作线程销毁栈上的 EventLoop 并 join 回收，无需在线程池中 delete loop。
}

inline void EventLoopThreadPool::start(ThreadInitCallback cb) {
    baseLoop_->assertInLoopThread();
    if (startAttempted_)
        throw std::logic_error("EventLoopThreadPool can only be started once");

    // 在执行初始化回调前记录，避免回调中重入 start() 或修改线程数。
    startAttempted_ = true;
    try {
        threads_.reserve(numThreads_);
        loops_.reserve(numThreads_);
        // 根据数量创建 subloop 线程池
        for (int i = 0; i < numThreads_; ++i) {
            std::string buf = name_ + std::to_string(i);
            auto t = std::make_unique<EventLoopThread>(cb, buf);
            threads_.push_back(std::move(t));
            loops_.push_back(threads_.back()->startLoop());
        }

        // 整个服务端只有一个线程, 运行 baseloop
        if (numThreads_ == 0 && cb)
            cb(baseLoop_);
        started_ = true;
    } catch (...) {
        loops_.clear();
        threads_.clear(); // 停止并回收已经创建的工作线程，再传播原异常
        throw;
    }
}

inline EventLoop *EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    NETLIB_ASSERT(started_);

    EventLoop *loop = baseLoop_;
    if (!loops_.empty()) { // 轮询
        loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
    }

    // 如果没有 subloop, 则一直返回 baseLoop_
    // 否则轮询返回 subloop
    return loop;
}

inline std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    baseLoop_->assertInLoopThread();
    NETLIB_ASSERT(started_);

    if (loops_.empty()) // 服务端仅一个 loop, 即 baseLoop_
        return std::vector<EventLoop *>(1, baseLoop_);
    else // 否则返回 subloop
        return loops_;
}

} // namespace netlib

#endif // NETLIB_EVENTLOOPTHREADPOOL_H
