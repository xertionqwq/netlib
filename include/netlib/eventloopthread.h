#ifndef NETLIB_EVENTLOOPTHREAD_H
#define NETLIB_EVENTLOOPTHREAD_H

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "netlib/eventloop.h"
#include "netlib/noncopyable.h"
#include "netlib/thread.h"

namespace netlib {

// 由同一控制线程启动和销毁，不支持重复启动或在工作线程中销毁。
class EventLoopThread : private Noncopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    explicit EventLoopThread(ThreadInitCallback cb = ThreadInitCallback(),
                             const std::string &name = std::string());
    ~EventLoopThread();

    // 等待初始化完成后返回借用指针，仅在工作线程的 EventLoop 存活时有效。
    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;                 // 工作线程栈上的对象，不负责释放
    bool exited_;                     // 防止循环快速退出后启动方一直等待
    ThreadInitCallback callback_;
    std::string name_;
    std::mutex mtx_;                  // 保护 loop_ 和 exited_
    std::condition_variable cv_;
    std::unique_ptr<Thread> thread_;  // startLoop() 中构造即启动
};

inline EventLoopThread::EventLoopThread(ThreadInitCallback cb, const std::string &name)
    : loop_(nullptr)
    , exited_(false)
    , callback_(std::move(cb))
    , name_(name) {
}

inline EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loop_)
            loop_->quit();
    }
    // 工作线程退出时也要获取 mutex_，因此不能持锁等待。
    if (thread_)
        thread_->join();
}

inline EventLoop* EventLoopThread::startLoop() {
    if (thread_)
        throw std::logic_error("EventLoopThread can only be started once");

    // 此时构造子线程, 构造即开启
    thread_ = std::make_unique<Thread>([this] { threadFunc(); }, name_);
    std::unique_lock<std::mutex> lock(mtx_);
    // 阻塞直到新线程构造完成或者运行完成
    cv_.wait(lock, [this] { return loop_ != nullptr || exited_; });
    if (exited_)
        throw std::runtime_error("EventLoop exited before startup completed");
    return loop_; // 将开始 loop 的指针返回
}

// 这个函数在单独的新线程里面运行
inline void EventLoopThread::threadFunc() {
    EventLoop loop; // 创建独立的 EventLoop, one loop per thread
    if (callback_)
        callback_(&loop); // 调用回调

    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_ = &loop;
    }
    cv_.notify_one();

    loop.loop();

    // loop 循环结束, 将 loop_ 指针置空, 并通知结束
    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_ = nullptr;
        exited_ = true;
    }
    cv_.notify_one();
}

} // namespace netlib

#endif // NETLIB_EVENTLOOPTHREAD_H
