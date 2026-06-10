# ucontext 协程原语学习笔记

> 日期：2026-06-10
> 阶段：协程预热 — 在裸 API 上理解上下文切换

## 核心认知

`<ucontext.h>` 提供四个函数，在用户态实现"保存当前执行状态 → 跳转到另一个执行状态"：

| API | 作用 |
|-----|------|
| `getcontext(ucontext_t *ucp)` | 将当前 CPU 寄存器 + 信号掩码快照到 `ucp` |
| `makecontext(ucontext_t *ucp, void(*func)(), int argc, ...)` | 给 `ucp` 绑定入口函数，必须在 `getcontext` + 分配栈之后调用 |
| `swapcontext(ucontext_t *oucp, ucontext_t *ucp)` | 原子操作：保存当前上下文到 `oucp`，恢复 `ucp` 的上下文并跳转 |
| `setcontext(ucontext_t *ucp)` | 无条件跳转到 `ucp`，不保存当前位置（相当于 `swapcontext(NULL, ucp)`） |

## ucontext_t 结构

```c
typedef struct ucontext_t {
    ucontext_t *uc_link;       // 当前上下文结束后，自动切换到的下一个上下文
    sigset_t    uc_sigmask;    // 信号掩码
    stack_t     uc_stack;      // 独立栈（ss_sp + ss_size + ss_flags），仅对新协程有意义
    mcontext_t  uc_mcontext;   // 机器相关的寄存器状态，不透明
} ucontext_t;
```

## 两个关键理解（来自追问）

### 1. swapcontext 和 uc_link 不是重复设计

`swapcontext` 是**主动让出**（"我还没跑完，切出去一会儿"），`uc_link` 是**兜底返回**（"我跑完了，自动去下一个人那里"）。

```
swapcontext(&main, &child)   ← 主动跳到 child
  │
  ▼
fun() 执行中
  │  (如果需要主动让出)
  │  swapcontext(&child, &main)  ← 主动 yield 回 main
  │
  ▼
fun() return
  │
  │  如果 uc_link == &main → 自动 setcontext(&main)，回到上次 swapcontext 的下一行
  │  如果 uc_link == nullptr → 线程直接退出，main 里存的返回地址永远用不到
```

**关键认知**：swapcontext 存在 main 里的返回地址，必须有人"走过去"——要么是 `uc_link` 在 fun return 时自动走，要么是 `swapcontext` 主动走。两者都没有，main 里那个地址就烂掉了。

`uc_link` 的真正作用是构建**协程链**：A 结束 → B 结束 → C 结束 → 最终回到 main。

### 2. 为什么 child 需要 getcontext + 栈分配，而 main 不需要？

`child` 和 `main` 的身份不同：

| | child（新协程） | main（线程主上下文） |
|---|---|---|
| 身份 | 新建的执行环境 | 已有线程的"存档点" |
| 需要独立栈？ | ✅ 必须，malloc ~128KB | ❌ 复用线程栈 |
| `getcontext` 的作用 | 拿合法寄存器快照当初始化模板 | 由 `swapcontext` 在跳转瞬间**隐式完成** |
| `makecontext` | ✅ 在栈上压入入口函数 | ❌ 不需要 |

`getcontext(&child)` 不是为了"保存有意义的状态"，而是因为 `uc_mcontext` 是一块未初始化的内存——必须先拿到合法的寄存器值，`makecontext` 才能在此基础上修改（压入入口地址、设置栈指针）。而 `main` 的 `uc_mcontext` 是 `swapcontext` 在跳转瞬间原子写入的，所以不需要手动 `getcontext`。

**一句话**：child 是盖新房子，需要地基+墙+屋顶；main 只是在墙上贴个便签，swapcontext 帮你写好地址。

## 栈大小方案

对网络 IO 协程（读写 socket、解析协议头）的典型栈消耗：

| 默认值 | 1000 并发 | 10000 并发 | 适用场景 |
|--------|-----------|------------|----------|
| 128KB | 128 MB | 1.28 GB | 学习项目 / 中小规模 |

决定采用 **128KB 默认 + 构造函数可配**（sylar 路线），后续加保护页检测溢出，不做分段栈（过度工程化）。

## 踩坑记录

- `makecontext` 的入口函数期望 `void (*)(void)`，如果函数写了参数（如 `void fun(void *arg)`），用 C 强转压下去虽然编译通过，但是未定义行为——makecontext 不传参给 fun
- `makecontext` 只支持传 int 参数（通过 argc 指定个数），不能传指针
