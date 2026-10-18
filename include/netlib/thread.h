#ifndef NETLIB_THREAD_H_
#define NETLIB_THREAD_H_

#include <condition_variable>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "netlib/noncopyable.h"
#include "netlib/currentThread.h"
#include "netlib/log.h"

namespace netlib {

class Semaphore {
    /*
    ** 模拟信号量
    */
public:
    explicit Semaphore(int count = 0) : count_(count) {}

    // P 操作 — 获取资源，count==0 时阻塞
    void wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        while (count_ == 0) { // 需要重复验证, 防止假唤醒
            cv_.wait(lock);
        }
        --count_;
    }

    // V 操作 — 释放资源，唤醒一个等待者
    void signal() {
        std::unique_lock<std::mutex> lock(mtx_);
        ++count_;
        cv_.notify_one();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    int count_;
};

class Thread : private Noncopyable {
    /*
    ** 包装自实现线程
    */
public:
    using FunCallback = std::function<void()>;

    explicit Thread(FunCallback cb, const std::string &name = std::string());
    ~Thread();

    pid_t getId() const { return id_; }
    const std::string &getName() const { return name_; }

    void join() { thread_.join(); }

public:
    // 获取系统分配的线程 id
    static pid_t GetThreadId() {
        return CurrentThread::tid();
    }
    // 获取当前所在线程
    static Thread *GetThis();   

    // 获取当前线程名字
    static const std::string &GetName();
    // 设置当前线程名字
    static void SetName(const std::string &name);
    // 获取当前创建的线程数
    static int numCreated() { return numCreated_; }

private:
    // 设置线程默认名称
    void setDefaultName();

private:
    pid_t id_;              // 进程 id
    std::thread thread_;    // 线程, 不可拷贝
    FunCallback cb_;        // 线程需要运行的函数
    std::string name_;      // 线程名
    Semaphore sem_;         // 引入信号量完成线程同步创建

    static std::atomic<int> numCreated_; // 记录已创建线程个数
};

inline std::atomic<int> Thread::numCreated_(0);

inline thread_local Thread *threadPtr = nullptr;       // 当前 Thread 对象指针
inline thread_local std::string threadName = "UNKNOWN"; // 当前线程名称

inline Thread::Thread(FunCallback cb, const std::string &name)
    : id_(0)
    , cb_(std::move(cb))
    , name_(name) {
    if (!cb_) {
        throw std::invalid_argument("Thread callback must not be empty");
    }
    setDefaultName();
    thread_ = std::thread([this]() { // 创建新的子线程
        threadName = name_;   // 在新线程的 TLS 里设
        threadPtr  = this;    // 在新线程的 TLS 里设
        id_ = GetThreadId();
        sem_.signal();        // 通知构造函数 id 就绪
        cb_();                // 执行用户回调
    });
    sem_.wait();              // 阻塞直到新线程完成初始化
}

inline Thread::~Thread() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

inline Thread *Thread::GetThis() {
    return threadPtr;
}

inline const std::string &Thread::GetName() {
    return threadName;
}

inline void Thread::SetName(const std::string &name) {
    if (threadPtr)
        threadPtr->name_ = name;
    threadName = name;
}

inline void Thread::setDefaultName() {
    const int num = ++numCreated_;
    if (name_.empty()) {
        name_ = "Thread " + std::to_string(num);
    }
}

} // namespace netlib

#endif // _NETLIB_THREAD_H_
