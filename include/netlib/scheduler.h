#ifndef NETLIB_SCHEDULER_H
#define NETLIB_SCHEDULER_H

#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

#include "netlib/fiber.h"
#include "netlib/thread.h"
// #include "netlib/hook.h"

namespace netlib{


class Scheduler {
    /*
    ** 调度器, 统筹管理线程与协程, M:N 模型
    ** 父子线程 -> 1:M 模型
    ** 调度与被调度协程 -> 1:N 模型
    ** 其中子线程就是调度协程
    ** 子协程就是被调度协程
    */

public:
    using FiberPtr = std::shared_ptr<Fiber>;
    using ThreadPtr = std::shared_ptr<Thread>;
    using FunCallBack = std::function<void()>;

public:
    // threads 指定线程池数量, useCaller 指定是否将主线程作为工作线程, name 设置调度器名称
    Scheduler(size_t threads = 1, bool use_Caller = true, const std::string name = "Scheduler");
    virtual ~Scheduler();

    const std::string &getName() const { return name_; }

public:
    static Scheduler *GetThis();

protected:
    void SetThis();

public:
    // 添加任务到任务队列
    // FiberOrCb -> 调度任务类型, 可以是协程对象或函数指针
    // 即我们希望可以执行协程任务或者直接的函数任务
    template <class FiberOrCb>
    void scheduleLock(FiberOrCb fc, pid_t thread = -1) {
        bool needTickle;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            needTickle = tasks_.empty();
            ScheduleTask task(fc, thread); // 创建任务对象
            if (task.fiber_ || task.cb_)    // 有任务, 加入
                tasks_.push_back(task);
        }
        if (needTickle) // 队列空了, 唤醒线程
            tickle();
    }

    virtual void start();    // 启动线程池, 启动调度器
    virtual void stop();     // 关闭线程池, 停止调度器, 等所有调度任务执行完后再返回

protected:
    virtual void tickle();  // 唤醒线程
    virtual void run();     // 线程函数
    virtual void idle();    // 空闲协程函数, 无任务调度时执行 idle 协程
    virtual bool stopping() {
        std::lock_guard<std::mutex> lock(mtx_);
        return stopping_ && tasks_.empty() && activeThreadCount_ == 0;
    } // 能否关闭
    // 当前调度协程进入 idle 时空闲线程数 +1, 从 idle 协程返回时空闲线程数 -1
    bool hasIdleThreads() { return idleThreadCount_ > 0; } // 返回是否又空闲线程

private:
    struct ScheduleTask { // 任务
        FiberPtr fiber_;
        FunCallBack cb_;
        pid_t thread_;

        ScheduleTask() {
            fiber_ = nullptr;
            cb_ = nullptr;
            thread_ = -1;
        }

        ScheduleTask(FunCallBack f, pid_t thr) {
            cb_ = std::move(f);
            thread_ = thr;
        }

        ScheduleTask(FunCallBack *f, pid_t thr) {
            cb_.swap(*f);
            thread_ = thr;
        }

        ScheduleTask(FiberPtr f, pid_t thr) {
            fiber_ = f;
            thread_ = thr;
        }

        ScheduleTask(FiberPtr *f, pid_t thr) {
            fiber_.swap(*f);
            thread_ = thr;
        }

        void reset() { // 重置
            fiber_ = nullptr;
            cb_ = nullptr;
            thread_ = -1;
        }
    };

private:
    std::string name_; // 调度器名称
    std::mutex mtx_;   // 互斥锁, 用于保护任务队列

    std::vector<ThreadPtr> threads_;  // 线程池, 存放初始化好的线程
    std::vector<ScheduleTask> tasks_; // 任务队列
    std::vector<int> threadIds_;      // 存储工作线程的线程 id

    size_t threadCount_ = 0;                      // 额外创建的线程数
    std::atomic<size_t> activeThreadCount_ = {0}; // 活跃线程数
    std::atomic<size_t> idleThreadCount_ = {0};   // 空闲线程数

    FiberPtr schedulerFiber_; // 是主线程 -> 创建调度协程

    bool useCaller_;        // 主线程是否为工作线程
    bool stopping_ = false; // 是否正在关闭
    int rootThread_ = -1;   // 是主线程 -> 记录主线程 id
};

inline thread_local Scheduler *schedulerPtr = nullptr;

Scheduler *Scheduler::GetThis() {
    return schedulerPtr;
}

void Scheduler::SetThis() {
    schedulerPtr = this;
}

Scheduler::Scheduler(size_t threads, bool useCaller, const std::string name) : 
    useCaller_(useCaller), name_(name) {
    assert(threads > 0 && Scheduler::GetThis() == nullptr);

    if (useCaller) {
        SetThis(); // 主线程作为工作线程时才注册
    }

    Thread::SetName(name);
    // 将主线程作为工作线程, 实现更高效的任务调度和管理
    if (useCaller) { // 若 useCaller 为 true, 则当前线程也为工作线程
        threads--;
        Fiber::GetThis(); // 创建主协程（GetThis 内部设 schedulerFiber = mainFiber）
        // 创建调度协程——使用 runInScheduler=false, 走 mainFiber 路径
        schedulerFiber_.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        rootThread_ = Thread::GetThreadId();             // 获取主线程 ID
        threadIds_.push_back(rootThread_);
    }

    threadCount_ = threads;
    if (debug)
        NETLIB_LOG_INFO << "Scheduler::Scheduler() success";
}
Scheduler::~Scheduler() {
    assert(stopping() == true);
    if (GetThis() == this)
        schedulerPtr = nullptr;
    if (debug)
        NETLIB_LOG_INFO << "Scheduler::~Scheduler()";
}

// 初始化调度线程池
void Scheduler::start() {
    std::lock_guard<std::mutex> lock(mtx_);
    stopping_ = false; // 重置停止标志，支持重复启停

    assert(threads_.empty());
    threads_.resize(threadCount_); // 重置线程数量
    for (size_t i = 0; i < threadCount_; i++) {
        threads_[i].reset(new Thread(std::bind(&Scheduler::run, this), name_ + "_" + std::to_string(i)));
        threadIds_.push_back(threads_[i]->getId());
    }
}
void Scheduler::stop() {
    if (debug)
        NETLIB_LOG_INFO << "Scheduler::stop() starts in thread: " << Thread::GetThreadId();

    if (stopping())
        return;

    stopping_ = true;
    if (useCaller_) {
        assert(GetThis() == this);
    } else {
        assert(GetThis() != this);
    }

    // 使用 tickle 唤醒空闲线程, 防止相关线程永久阻塞在等待任务队列中
    for (size_t i = 0; i < threadCount_; i++) 
        tickle(); // 唤醒空闲线程

    if (schedulerFiber_) {
        // 直接在当前线程执行 run() 调度循环, 不经过协程切换
        run();
        if (debug)
            NETLIB_LOG_INFO << "Scheduler::stop() ends in threadId: " << Thread::GetThreadId();
    }
    std::vector<ThreadPtr> thrs; // 通过 swap 获取线程, 方便 join 退出
    {
        std::lock_guard<std::mutex> lock(mtx_);
        thrs.swap(threads_);
    }
    for (auto &t : thrs) {
        t->join();
    }

    if (debug)
        NETLIB_LOG_INFO << "Scheduler::stop() ends in threadId: " << Thread::GetThreadId();
}

void Scheduler::tickle() {

}
// 调度器核心, 负责从任务队列中取出任务并用协程执行
void Scheduler::run() {
    auto threadId = Thread::GetThreadId();
    if (debug)
        NETLIB_LOG_INFO << "Scheduler::run() starts in thread: " << threadId;

    // set_hook_enable(true);
    SetThis();

    if (threadId != rootThread_) { // 不是主线程, 则创建主协程
        Fiber::GetThis();
    }
    // 创建空闲子协程
    FiberPtr idleFiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
    ScheduleTask task;

    while (true) {
        task.reset();
        bool tickleMe = false; // 是否唤醒其他线程进行任务调度

        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = tasks_.begin();
            // 1-遍历任务队列
            while (it != tasks_.end()) {
                if (it->thread_ != -1 && it->thread_ != threadId) {
                    // 线程存活并且不是当前 threadId, 让任何线程可以执行任务
                    it++;
                    tickleMe = true;
                    continue;
                }
                // 2-取出任务
                assert(it->fiber_ || it->cb_);
                task = *it;
                tasks_.erase(it);
                activeThreadCount_++;
                break; // 取到任务, 直接 break
            }
            // 确保仍然有未处理的任务
            tickleMe = tickleMe || (it != tasks_.end());
        }

        if (tickleMe)
            tickle();

        // 3-执行任务
        if (task.fiber_) {
            {
                std::lock_guard<std::mutex> lock(task.fiber_->mtx_);
                if (task.fiber_->getSate() != Fiber::State::TERM)
                    task.fiber_->resume(); // 切换至子协程执行任务
            }
            activeThreadCount_--; // 执行完任务后协程不再活跃

            Fiber::State st = task.fiber_->getSate();
            if (st == Fiber::State::READY)
                scheduleLock(task.fiber_, task.thread_); // 重新入队
            else if (st != Fiber::State::TERM
                     && st != Fiber::State::EXCEPT) {
                // HOLD -- 挂起, 等待外部事件, 暂时不动
            }
            task.reset();
        } else if (task.cb_) {
            // 封装为协程加入调度
            FiberPtr cbFiber = std::make_shared<Fiber>(task.cb_);
            {
                std::lock_guard<std::mutex> lock(cbFiber->mtx_);
                cbFiber->resume();
            }
            activeThreadCount_--;

            Fiber::State st = cbFiber->getSate();
            if (st == Fiber::State::READY)
                scheduleLock(cbFiber, task.thread_); // 重新入队
            else if (st != Fiber::State::TERM
                        && st != Fiber::State::EXCEPT) {
                // HOLD -- 挂起, 等待外部事件, 暂时不动
            }
            task.reset();
        } else {
            // 4-无任务, 执行空闲协程 
            if (idleFiber->getSate() == Fiber::State::TERM) {
                if (debug)
                    NETLIB_LOG_INFO << "Scheduler::run() ends in thread: " << threadId;
                break; // 退出 run 循环
            }
            // 若调度器没有调度任务, idle 协程不断进行 resume/yield
            // 不会结束而是进入忙等
            // 若调度器停止了, 会进入上述的 if/else 处理任务
            idleThreadCount_++;
            idleFiber->resume();
            idleThreadCount_--;
        }
    }
}
void Scheduler::idle() {
    while (!stopping()) {
        if (debug)
            NETLIB_LOG_INFO << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId();
        sleep(1);
        Fiber::GetThis()->yield();
    }
}

} // netlib

#endif // NETLIB_SCHEDULER_H