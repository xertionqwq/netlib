#ifndef NETLIB_THREAD_H_
#define NETLIB_THREAD_H_

#include <condition_variable>
#include <string>
#include <mutex>
#include <thread>
#include <functional>
#include <sys/syscall.h>
#include <unistd.h>

#include "log.h"

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

class Thread {
    /*
    ** 包装自实现线程
    */
public:
    using FunCallback = std::function<void()>;

    Thread(FunCallback cb, const std::string &name);
    ~Thread();

    pid_t getId() const { return id_; }
    const std::string &getName() const { return name_; }

    void join() { thread_.join(); }

public:
    // 获取系统分配的线程 id
    static pid_t GetThreadId() {
        return syscall(SYS_gettid);
    }
    // 获取当前所在线程
    static Thread *GetThis();   

    // 获取当前线程名字
    static const std::string &GetName();
    // 设置当前线程名字
    static void SetName(const std::string &name);

private:
    pid_t id_ = -1;      // 进程 id
    std::thread thread_; // 线程, 不可拷贝
    FunCallback cb_;     // 线程需要运行的函数
    std::string name_;   // 线程名
    Semaphore sem_;      // 引入信号量完成线程同步创建
};

inline thread_local Thread *threadPtr = nullptr;       // 当前 Thread 对象指针
inline thread_local std::string threadName = "UNKNOWN"; // 当前线程名称

Thread::Thread(FunCallback cb, const std::string &name) : cb_(std::move(cb)), name_(name) {
    thread_ = std::thread([this]() {
        threadName = name_;   // 在新线程的 TLS 里设
        threadPtr  = this;    // 在新线程的 TLS 里设
        id_ = GetThreadId();
        sem_.signal();        // 通知构造函数 id 就绪
        cb_();                // 执行用户回调
    });
    sem_.wait();              // 阻塞直到新线程完成初始化
}

Thread::~Thread() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

Thread *Thread::GetThis() {
    return threadPtr;
}

const std::string &Thread::GetName() {
    return threadName;
}

void Thread::SetName(const std::string &name) {
    if (threadPtr)
        threadPtr->name_ = name;
    threadName = name;
}

} // namespace netlib

#endif // _NETLIB_THREAD_H_