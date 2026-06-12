# Fiber 类 — ucontext 栈满协程封装

> 日期：2026-06-12
> 阶段：Phase 2 协程核心 — 从裸 ucontext API 到可调度执行体

## 基础介绍

Fiber 提供协程的基本功能——一段可以暂停和恢复执行的函数。底层使用 `ucontext_t`（非对称协程）实现上下文切换：`getcontext` 保存寄存器快照，`makecontext` 绑定入口函数，`swapcontext` 原子切换执行流。用户通过 `std::function<void()>` 传入回调函数，Fiber 负责在独立栈上执行它，并在必要时挂起/恢复。

## 设计决策

### 命名：fiber.h vs coroutine.h

**选择 `fiber.h`**。Fiber 指具体的执行体（一段独立栈 + 一个上下文），Coroutine 是概念泛指。后续 Scheduler 调度的是 Fiber 实例，关系自明。

### 非对称协程

调用者是 Scheduler（或主线程），被调度者是 Fiber。Fiber 只负责"我能暂停和恢复"，切回谁由外部决定。

### runInScheduler 双模式

同一个 Fiber 类支持两种运行环境：

| | runInScheduler = true | runInScheduler = false |
|---|---|---|
| 跳回目标 | schedulerFiber（Scheduler 调度协程） | mainFiber（线程主协程） |
| 用途 | 生产：Scheduler 驱动 | 测试/调试：裸 resume/yield |
| 好处 | | 无 Scheduler 也能验证状态机 |

通过 `resume()` 和 `yield()` 中的分支实现——swapcontext 的目标上下文不同，其余逻辑一致。

## 实现核心

### 三个 thread_local 变量

```
fiberPtr        → 当前执行的 Fiber*（裸指针）
mainFiber       → 线程根协程（shared_ptr，TLS 持有生命周期）
schedulerFiber  → 调度协程 Fiber*（裸指针，默认指向 mainFiber）
```

`GetThis()` 首次调用时创建 mainFiber → schedulerFiber 默认指向它 → resume/yield 以此为跳回目标。

### 两种构造

| | 主协程（Fiber() private） | 子协程（Fiber(cb, sz, sched)） |
|---|---|---|
| 栈 | nullptr（复用线程栈） | malloc(128KB) |
| 初始状态 | RUNNING | READY |
| getcontext | 快照线程上下文 | 初始化 uc_mcontext 模板 |
| makecontext | 不调用 | 绑定 MainFunc 入口 |
| 生命周期 | 跟随线程 | Scheduler/用户管理 |

### 状态机

```
构造 → READY ──resume()──→ RUNNING
                ↑              │
                │   yield()    │ cb_()正常 → TERM
                └──────────────┤ cb_()异常 → EXCEPT
                               │
                HOLD（等IO）    │
                               ↓
                          yield() → 调度器检查状态
                          不入队 → 销毁
```

### MainFunc — C/C++ 胶水层

```cpp
static void MainFunc() {
    auto curr = GetThis();       // 从 TLS 恢复 C++ 对象
    try {
        curr->cb_();             // 执行用户回调
        curr->state_ = TERM;
    } catch (...) {
        curr->state_ = EXCEPT;   // 异常不传播到 ucontext
    }
    auto raw = curr.get();
    curr.reset();                // 释放 shared_ptr 引用
    raw->yield();                // 走后不回
}
```

`makecontext` 只能接受裸函数指针，不能接受 `std::function`——MainFunc 就是桥接层，和 Thread 的 `static void* run(void* arg)` 一个道理。

### resume/yield 对称

```
resume():  调度者 → Fiber    swapcontext(&caller.ctx_, &fiber.ctx_)
yield():   Fiber → 调度者    swapcontext(&fiber.ctx_, &caller.ctx_)
```

同一个 swapcontext，方向相反，存谁读谁互换。

### uc_link = nullptr 的设计

yield 是主动切回（可控），uc_link 是被动兜底（函数 return 后自动跳）。设置 `nullptr` 而非 `&schedulerFiber` 的原因是：如果走 uc_link 路径意味着代码有 bug（忘了 yield），`nullptr` → crash → 立刻发现，比默默切回调度器掩盖问题好。

### curr.reset() + raw->yield() 的生命周期

```cpp
auto raw = curr.get();   // 保存裸指针
curr.reset();            // 释放 MainFunc 内的引用
raw->yield();            // swapOut，走后不回
```

yield 不返回。如果 reset 放在 yield 之后，引用永远不会释放。reset 之后 fiber 是否存活取决于 Scheduler 还持有它的 shared_ptr。裸指针不增加引用计数，但 Scheduler 的引用让对象活着。

## 踩坑记录

1. **`malloc(stackSize)` 误用参数 0**。构造函数参数默认 0（哨兵值），三元式 `stackSize_ = stackSize ? stackSize : 128000` 之后，`malloc` 应取成员 `stackSize_` 而非参数 `stackSize`。使用参数导致 `malloc(0)` → swapcontext SIGSEGV。
2. **yield() 忘记改状态**。用户主动 yield 后状态仍为 RUNNING，再次 resume 时 `assert(state_ == READY)` 失败。修复：yield 中将 RUNNING → READY，但 TERM/EXCEPT 保持不变。
3. **reset() 漏掉 makecontext**。getcontext 之后必须重新 makecontext，否则 swapcontext 跳转到没有入口的上下文。
4. **getcontext/swapcontext 错误处理用 if/try-catch 不合适**。ucontext 是 C API，不抛 C++ 异常，失败意味着不可恢复的 bug。改为 `assert(getcontext(&ctx_) != -1)`。

## 与后续模块的关系

Fiber 是 Scheduler 的"执行单元"。Scheduler 维护就绪队列，`run()` 循环从中取 Fiber → resume → 检查状态 → 决定是否重新入队。Fiber 不需要知道调度策略，只需要知道"切回调度器"。
