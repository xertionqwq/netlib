# Thread 类 — std::thread 封装

> 日期：2026-06-11
> 阶段：Phase 1 基础设施 — 线程封装与同步原语

## 设计决策

### pthread vs std::thread

| | pthread | std::thread |
|---|---|---|
| 构造/运行 | `pthread_create` 分离"创建"和"运行" | 构造即启动，不可拷贝 |
| 同步初始化 | 可在 run 回调里先设成员再 signal（sylar 模式） | 必须在 lambda 里手动同步时序 |
| 线程 ID | `pthread_t` 或 `syscall(SYS_gettid)` | `native_handle()` 拿 pthread_t，tid 仍需 syscall |
| RAII | 手动 join/detach | 析构时若 joinable 则 terminate，需自行管理 |
| 代码量 | 更多样板 | 更少，lambda 替代 static run |
| 个人熟练度 | — | 已有 C++11 线程池经验 |

**选择 std::thread**：熟悉度高 + 代码更少 + lambda 捕获 `this` 可替代 `static void* run(void* arg)`。

### 组合 vs 继承

**选择组合（内部 `std::thread thread_` 成员）**：

- 继承要求父类在初始化列表构造 → `std::thread` 立刻启动，此时 Semaphore 等成员尚未初始化，无法做同步等待
- 组合允许在构造函数体内创建和启动线程，所有成员已就绪
- 语义更准确：Thread 是一个控制器，不是一种特殊的 thread

### Semaphore 同步启动

核心问题：`std::thread` 构造即启动，构造函数返回时无法保证新线程已经运行到关键初始化代码。

解决方案：构造函数体内创建线程 → lambda 完成初始化后 `sem_.signal()` → 构造函数 `sem_.wait()` 阻塞直到初始化完成。返回值保证 `id_` 和 `threadPtr` 已就绪。

## 实现要点

### TLS 管理（thread-local storage）

```cpp
inline thread_local Thread* threadPtr = nullptr;     // 当前线程的 Thread 对象
inline thread_local std::string threadName = "UNKNOWN"; // 当前线程名
```

- `thread_local`：每个线程一份独立副本
- `inline`（C++17）：消除头文件中多翻译单元的 ODR 冲突。若用 `static`，每个 `.cpp` 各自有一份独立副本——`test_a.cpp` 设的 `threadPtr` 在 `test_b.cpp` 中看不到

### 初始化时序

```
调用者线程                      新线程
──────────                    ──────────
构造 Thread 对象
  初始化列表：sem_(0)
  函数体：
    thread_ = std::thread(
      [this]() {
        ① threadName = name_
        ② threadPtr = this        ← 新线程的 TLS
        ③ id_ = GetThreadId()     ← syscall(SYS_gettid)
        ④ sem_.signal()           ← 通知"我初始化完了"
        ⑤ cb_()                   ← 执行用户回调
      })
    sem_.wait() ←─── 阻塞 ──→ ④ 解除阻塞
    此时 id_ 一定有效 ✓
```

### 接口一览

| 接口 | 类型 | 说明 |
|------|------|------|
| `Thread(cb, name)` | 构造 | 启动线程，阻塞至 id 就绪 |
| `~Thread()` | 析构 | join 线程 |
| `getId()` | 实例 | 返回 `pid_t`（Linux TID） |
| `getName()` | 实例 | 返回构造时名字 |
| `join()` | 实例 | 等待线程结束 |
| `GetThreadId()` | 静态 | 返回当前线程 TID（`syscall(SYS_gettid)`） |
| `GetThis()` | 静态 | 返回当前线程的 `Thread*` |
| `GetName()` | 静态 | 返回当前线程名（tls） |
| `SetName(name)` | 静态 | 设置当前线程名（同步更新 Thread 对象 + tls） |

## 关键教训

1. **头文件中的 `thread_local` 变量必须加 `inline`，不能加 `static`**。`static thread_local` 导致每个 `.cpp` 有独立副本，`GetThis()` 跨翻译单元不可见。
2. **信号量（Semaphore）是 `mutex + cv + count` 的语义化封装**。裸 cv 需要额外 bool 标志，Semaphore 直接表达"等待 N 次 signal"，可复用性远高于裸 cv。
3. **std::thread 构造即运行 + 不可拷贝** 的特性要求通过 lambda 和同步原语控制时序，pthread 的"先创建再运行"模式在 std::thread 中不存在。

## 对后续模块的意义

Thread 是 Scheduler 的基础依赖。Scheduler::start() 创建 N 个 worker 线程，每个线程 run() 循环从就绪队列取 Fiber 执行。Thread 的同步构造保证 worker 线程启动后即可安全调度。
