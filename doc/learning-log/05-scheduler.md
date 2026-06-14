# Scheduler 类 — N-M 线程池协程调度器

> 日期：2026-06-15
> 阶段：Phase 2 协程核心 — 从单个 Fiber 到多线程并发调度

## 基础介绍

Scheduler 是协程执行引擎，实现 N-M 调度模型——N 个内核线程上运行 M 个协程。用户通过 `scheduleLock()` 提交任务（可以是 Fiber 对象或裸回调），Scheduler 在线程池中找到空闲线程执行它。协程 yield 后根据状态决定重新入队、挂起或销毁。

## 架构

```
父线程（main）
  │
  ├─ 构造 → scheduleLock 塞任务
  ├─ start() → 创建 N 个 Thread，每个进入 run()
  │                           │
  │                    调度协程（主协程）
  │                     ┌────┴──────────┐
  │                     │ idleFiber     │ 队列空时忙等
  │                     │ f1, f2, ...   │ 被调度子协程
  │                     └───────────────┘
  │                           │
  ├─ stop() → stopping_=true → idle 检测退出 → join
  └─ 析构
```

两层管理：
- **线程层（1:N）**：`start()`/`stop()` 控制 Thread 池创建和 join
- **协程层（N:M）**：`run()` 循环从共享队列取任务 → resume → 状态判定

## 核心设计

### ScheduleTask — 统一 Fiber 和回调

```cpp
struct ScheduleTask {
    FiberPtr fiber_;   // 已有 Fiber 对象
    FunCallBack cb_;   // 或裸回调
    pid_t thread_;     // 线程绑定（-1 表示任意）
};
```

用户只看到一个 `scheduleLock()`，传入什么类型就按对应路径执行。

### run() 调度循环

```
while (true):
    ┌─ 从 tasks_ 取一个任务（跳过绑定其他线程的）
    ├─ 有 Fiber  → fiber->resume()
    │               │
    │               ├─ TERM  → 不重新入队，销毁
    │               ├─ EXCEPT → 不重新入队，销毁
    │               ├─ READY → scheduleLock 重新入队
    │               └─ else → HOLD，等外部事件
    │
    ├─ 有回调  → new Fiber(cb) → resume（同上）
    │
    └─ 队列空 → idleFiber->resume() → idle() 忙等
                  │
                  stopping()?
                  ├─ false → sleep(1) + yield 循环
                  └─ true  → return → idleFiber TERM → run() break
```

### idle / tickle — 空转和唤醒

```
基类 Scheduler:
  idle()   → sleep(1) 轮询 + yield    （延迟最多 1 秒）
  tickle() → 空实现                    （轮询模式不需要）

IOManager（后续）:
  idle()   → epoll_wait               （零延迟）
  tickle() → write(pipe, "T", 1)      （即时唤醒）
```

基类提供可工作的轮询默认行为，IOManager 覆写为高效 epoll 实现。这正是 `virtual` 的意图。

### stopping 三件套

| 变量/函数 | 角色 |
|-----------|------|
| `stopping_` | 标志位，`stop()` 设为 true |
| `stopping()` | 条件检查：`stopping_ && tasks_.empty() && activeThreadCount_ == 0` |
| `stop()` | 发起者，设标志 → tickle 唤醒 → join 等待 |

AND 逻辑保证优雅关闭：不丢任务、不打断正在执行的协程。

### useCaller — 主线程参与调度

`useCaller=true` 时主线程作为工作线程之一，`threadCount_` 少创建一个。`stop()` 中直接同步调用 `run()` 而非协程切换——避免了 schedulerFiber 自己和自己的 swapcontext 冲突。

## 依赖关系

```
Scheduler
├── Fiber   — 执行体（resume / yield / 状态机）
├── Thread  — 线程池（start / join）
└── Mutex   — 保护 tasks_ 就绪队列（std::mutex）
```

Fiber 不知道自己被调度；Scheduler 不知道 Fiber 内部怎么切换。两者通过 `state_` 通信。

## 踩坑记录

1. **yield 后不重新入队**：`run()` 中 `resume()` 返回后直接 `task.reset()`，yield 的 Fiber 丢了。修复：检查 `state_ == READY` → `scheduleLock` 重新入队。

2. **`scheduleLock` 内调用 `scheduleLock` 死锁**：重入队时在 `mtx_` 锁内调了自己 → `std::mutex` 不可递归。修复：状态检查移到锁外。

3. **schedulerFiber 自引用**：构造中 `SetSchedulerFiber(schedulerFiber_.get())` 导致 `schedulerFiber_->resume()` 时 `swapcontext(&self->ctx_, &self->ctx_)`——自己和自己交换。修复：删掉该行 + `schedulerFiber_` 用 `runInScheduler=false` + `stop()` 直接调 `run()`。

4. **`stopping_` 未重置**：第二次 `start()` 时 `stopping_` 仍为 true，直接 return。修复：`start()` 开头重置 `stopping_ = false`。

5. **cb 路径用错变量**：检查 `task.fiber_->getSate()` 但这是回调路径（`task.fiber_` 是 nullptr）。修复：用 `cbFiber`。

## 与后续模块的关系

Scheduler 是 IOManager 的基类。IOManager 继承 Scheduler 并覆写 `idle()`（epoll_wait）和 `tickle()`（pipe write），将"轮询找活干"升级为"事件驱动找活干"。Timer 也通过覆写 `stopping()` 加入定时器检查。
