#ifndef NETLIB_FIBER_H
#define NETLIB_FIBER_H

#include <cassert>
#include <memory>
#include <atomic>
#include <functional>
#include <ucontext.h>

#include "netlib/log.h"

namespace netlib {

static bool debug = false;

class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    using FunCallback = std::function<void()>;

    // 定义协程状态
    enum class State {
        READY,
        RUNNING,
        HOLD,
        TERM,
        EXCEPT
    };

private:
    Fiber(); // 将无参构造设为私有, 用于创建主协程

public:
    // 创建制定协程, 参数有回调函数 栈大小 是否参与调度器调度
    Fiber(FunCallback cb, size_t stackSize = 0, bool runInScheduer = true);
    ~Fiber();

    // 重置协程状态和入口函数, 相当于复用协程栈空间
    void reset(FunCallback cb);
    void resume(); // 恢复执行
    void yield();  // 让出执行给调度协程

    uint64_t getId() const { return id_; }
    State getSate() const { return state_; }

public:
    static void SetThis(Fiber *f);           // 设置当前运行的协程
    static std::shared_ptr<Fiber> GetThis(); // 获取当前协程的共享实例
    static void SetSchedulerFiber(Fiber *f); // 设置调度协程(默认主协程)
    static uint64_t GetFiberId();            // 获取当前运行的协程的 ID
    static void MainFunc();                  // 协程主函数, 入口点

private:
    uint64_t id_ = 0;            // 协程唯一标识符
    uint32_t stackSize_ = 0;     // 栈的大小
    State state_ = State::READY; // 协程初始态设为 ready
    ucontext_t ctx_;             // 协程上下文
    void *stack_ = nullptr;      // 协程栈的指针
    FunCallback cb_;             // 协程的回调函数
    bool runInScheduler_;        // 标志->是否将执行器交给协程

public:
    std::mutex mtx_;
};

// 线程上有关协程的信息
// 正在运行的协程
inline thread_local Fiber *fiberPtr = nullptr;
// 主协程
inline thread_local std::shared_ptr<Fiber> mainFiber = nullptr;
// 调度协程
inline thread_local Fiber *schedulerFiber = nullptr;

// 全局协程 ID 计数器
inline static std::atomic<uint64_t> fiberID{0};
// 活跃协程数量计数器
inline static std::atomic<uint64_t> fiberCount{0};

// 设置当前运行的协程
void Fiber::SetThis(Fiber *f) {
    fiberPtr = f;
}

// 创建主协程 设置状态, 初始化上下文并分配 ID
Fiber::Fiber() {
    SetThis(this);
    state_ = State::RUNNING;

    assert(getcontext(&ctx_) != -1);

    id_ = fiberID++; // 分配 id, 协程 id 由 0 开始
    fiberCount++;    // 活跃协程数 +1
    if (debug)
        NETLIB_LOG_INFO << "Fiber(): main id = " << id_;
}

// 创建新的协程 初始化回调函数, 栈大小和状态, 分配栈空间
Fiber::Fiber(FunCallback cb, size_t stackSize, bool runInScheduler) :
cb_(std::move(cb)), runInScheduler_(runInScheduler) {
    state_ = State::READY;
    // 分配 128k, 应对当前环境足以
    stackSize_ = stackSize ? stackSize : 128000;
    stack_ = malloc(stackSize_);

    assert(getcontext(&ctx_) != -1);

    ctx_.uc_link = nullptr;
    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stackSize_;
    makecontext(&ctx_, &Fiber::MainFunc, 0);

    id_ = fiberID++;
    fiberCount++;
    if (debug)
        NETLIB_LOG_INFO << "Fiber(): child id = " << id_;
}

Fiber::~Fiber() {
    fiberCount--;
    if (stack_)
        free(stack_);
    if (debug)
        NETLIB_LOG_INFO << "~Fiber(): id = " << id_;
}

// 重置协程回调函数和上下文, 使用与将协程状态由`TERM`重置为`READY`
void Fiber::reset(FunCallback cb) {
    assert(stack_ && state_ == State::TERM);

    state_ = State::READY;
    cb_ = std::move(cb);

    assert(getcontext(&ctx_) != -1);

    ctx_.uc_link = nullptr;
    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stackSize_;
    makecontext(&ctx_, &Fiber::MainFunc, 0);
}

// 将协程设置为 running, 并恢复协程的执行
void Fiber::resume() {
    assert(state_ == State::READY);
    state_ = State::RUNNING;

    if (runInScheduler_) {
        SetThis(this);
        assert(swapcontext(&(schedulerFiber->ctx_), &ctx_) != -1);
    } else {
        SetThis(this);
        assert(swapcontext(&(mainFiber->ctx_), &ctx_) != -1);
    }
}
// 将协程挂起, 恢复父协程上下文
void Fiber::yield() {
    assert(state_ == State::RUNNING || state_ == State::TERM || state_ == State::EXCEPT);
    if (state_ != State::TERM && state_ != State::EXCEPT)
        state_ = State::READY;

    if (runInScheduler_) {
        SetThis(schedulerFiber);
        assert(swapcontext(&ctx_, &(schedulerFiber->ctx_)) != -1);
    } else {
        SetThis(mainFiber.get());
        assert(swapcontext(&ctx_, &(mainFiber->ctx_)) != -1);
    }
}

// 首次执行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis() {
    if (fiberPtr)
        return fiberPtr->shared_from_this();

    std::shared_ptr<Fiber> main(new Fiber());
    mainFiber = main;
    schedulerFiber = main.get(); // 主协程默认为调度协程
    return main;
}

// 设置当前的调度协程
void Fiber::SetSchedulerFiber(Fiber *f) {
    schedulerFiber = f;
}

uint64_t Fiber::GetFiberId() {
    if (fiberPtr)
        return fiberPtr->getId();
    return (uint64_t)-1; // 表示错误
}

void Fiber::MainFunc() {
    auto curr = GetThis();
    assert(curr);

    try {
        curr->cb_();
        curr->cb_ = nullptr;
        curr->state_ = State::TERM;
    }
    catch(...) {
        curr->state_ = State::EXCEPT;
        NETLIB_LOG_ERROR << "Fiber::MainFunc(): id = " << curr->id_;
    }

    auto raw = curr.get(); // 执行完毕, 让出执行权
    curr.reset();          // 注意, 当前指向 nullptr, 意味着该协程可以再次被调用(reset)
    raw->yield();          // 挂起回到调度协程
}

} // namespace netlib


#endif // NETLIB_FIBER_H