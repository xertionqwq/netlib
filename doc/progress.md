# 项目进度

更新日期：2026-10-17。

## 当前方向

当前优先沿 muduo 路线学习和完善 Reactor 网络库，逐步补齐线程封装、连接接入和服务端组织。协程相关代码已有学习原型，后续再讨论与网络层整合，不要求当前按旧的四阶段协程迁移顺序推进。

项目使用 C++17、Linux/POSIX 接口，无第三方库依赖。目标保持 header-only；现有学习原型仍需逐步审查，不能视为全部完成工程化。

## 模块状态

| 模块 | 状态 | 当前实现与边界 |
|---|---|---|
| Noncopyable | 已实现 | 统一禁止资源类拷贝；Thread、EventLoopThread 等私有继承 |
| Logger / Timestamp | 已实现 | 同步流式日志、级别过滤、errno 保存、FATAL 和项目断言；Timestamp 获取秒级时间并格式化本地时间 |
| CurrentThread / Thread | 基本实现完成 | 缓存 Linux TID；Thread 构造即启动、同步发布 TID、默认命名、累计计数、拒绝空回调、join 回收 |
| Channel | 核心实现完成 | 管理关注事件和回调、传递读事件时间戳、支持 tie 与处理期间的生命周期保护 |
| Poller / EPollPoller | 核心实现完成 | Poller 是抽象基类；EPollPoller 实现 epoll_wait、活跃事件收集、事件数组扩容及注册状态管理 |
| 默认 Poller 工厂 | 已实现 | `defaultPoller.h` 选择 EPollPoller；尚无 poll 后端，相关环境变量会报告不支持 |
| EventLoop | 基本实现完成 | one loop per thread、事件分发、待办任务队列、eventfd 跨线程唤醒和退出 |
| EventLoopThread | 最小实现 | 延迟到 startLoop 创建 Thread，等待循环初始化就绪，退出时停止循环并回收线程 |
| Buffer / InetAddress | 已有实现 | Buffer 管理收发缓冲区；InetAddress 当前只封装 IPv4，不支持 IPv6 |
| Acceptor | 待完善 | 已有监听与接入流程，需要调整监听时机、状态和错误处理 |
| TcpConnection | 待完善 | 已有读写缓冲、事件回调和关闭流程，跨线程接口与连接生命周期仍需补齐 |
| Fiber / Scheduler / Timer | 已有学习原型 | 已有单独测试和学习笔记，尚未与 Reactor 整合，跨翻译单元与 NDEBUG 适配仍需审查 |
| EventLoopThreadPool | 最小实现 | 管理工作循环、零线程回退和轮询分配；补齐参数与线程归属检查、启动失败清理，覆盖多翻译单元与 NDEBUG |
| TcpServer | 待实现 | 还未形成服务端连接管理层 |
| IOManager / Hook / Socket / Stream / ByteArray | 后续方向 | 尚未实现，是否引入及如何与 Reactor 协作届时再确定 |

Poller 的具体后端与基类已经分离，不再将 `poller.h` 描述为 epoll 的具体封装。Poller 保存 Channel 借用指针，不负责释放 Channel 或关闭其 fd。

## 已确定的线程方案

### Thread

- 保持构造即启动，不增加显式 `start()`；已有 Scheduler 继续使用该方式。
- 不维护 `started_`、`running` 或 `joined_`，join 状态由底层 `std::thread` 管理。
- 构造函数等待工作线程发布 TID；该同步不代表 EventLoop 已经创建或正在循环。
- 线程默认名称在启动前确定，与工作线程 TLS 名称一致；`numCreated()` 是累计计数，不是当前运行数。
- 空回调在启动和计数前抛出 `std::invalid_argument`；若只需创建不执行任务的线程，可传入 `[] {}`。
- 析构时仍可 join 的线程会被回收；线程对象不能在自身工作线程中销毁。

### EventLoopThread

- 构造函数仅保存配置，`startLoop()` 中创建 `unique_ptr<Thread>`，从而适配 Thread 构造即启动。
- 工作线程在栈上创建、运行并销毁 EventLoop；初始化回调可为空。
- 通过互斥锁和条件变量发布 `loop_`，初始化完成后启动方才能取得指针。
- `exited_` 表示循环已正常退出，避免快速退出后启动方一直等待；启动方观察到已退出时抛出 `std::runtime_error`。
- 析构时持锁检查指针并请求 `quit()`，释放锁后再 `join()`；即使循环已经退出，也要回收线程。
- 仅允许启动一次，重复启动抛出 `std::logic_error`；启动和析构由同一控制线程串行执行。
- 返回的 `EventLoop*` 是借用指针，仅在工作线程的 EventLoop 存活时有效；调用方不能删除它，也不能在循环退出后继续访问。

### EventLoopThreadPool

- 构造、配置、启动、查询和析构均在 baseLoop 所属线程执行；baseLoop 为借用指针，必须在线程池销毁后才能销毁。
- 构造时不创建工作线程；负线程数抛出 `std::invalid_argument`，启动后不再允许配置或重复启动。
- `start()` 按值接收可选初始化回调，等待各工作循环初始化完成；零工作线程时在 baseLoop 线程执行回调。
- `startAttempted_` 在启动前置位，阻止重入启动和修改配置；`started_` 仅在初始化全部成功后置位。
- 控制线程中的 `start()` 捕获异常后，清空借用指针并停止、回收已创建线程，再传播原异常；失败后 `started()` 返回 false，不支持重试、修改配置或获取循环。工作线程入口异常策略仍属于暂缓事项。
- 有工作线程时仅在工作循环之间轮询；零工作线程时使用 baseLoop。返回的循环指针均不转移所有权，使用期间不得让工作循环自行退出。
- 线程池销毁时由 EventLoopThread 的析构停止并回收工作线程，不负责停止或销毁 baseLoop。

## 后续推进建议

EventLoopThreadPool 的当前最小实现与验证已完成。以下是后续建议的学习顺序，不表示已经授权直接实现所有后续模块：

1. 调整 Acceptor：分离构造与 `listen()`，维护监听状态，完善接入 fd 的创建、交接和异常清理。
2. 完善 TcpConnection：接入 Channel tie，补齐跨线程发送与关闭，确保输出缓冲排空后再关闭写端。
3. 实现 TcpServer：组织 Acceptor、连接集合、连接创建和销毁。
4. 补充端到端网络验证，再评估协程原型的复习、修正与整合。

### 网络层已知待办

- Acceptor 当前在构造中完成 listen 和事件注册，但 `listening_` 未同步更新；需要明确启动接口。
- Acceptor 在检查 accept 返回值前调用 fcntl，可能覆盖 errno；后续考虑 accept4 并同时设置 NONBLOCK/CLOEXEC。
- Acceptor 未配置新连接回调时，需要关闭已接收的 fd；系统调用错误不能仅靠标准 assert 处理。
- TcpConnection 尚未在连接建立时设置 Channel tie；跨线程 `send()` 和 `shutdown()` 分支仍有 TODO。
- TcpConnection 的 shutdown 时机、状态访问和销毁所属线程需要一并审查，避免输出未排空就关闭写端。

## 暂缓事项

- Thread 执行入口及 EventLoopThread 初始化/循环异常的日志、传播和恢复策略。
- `Thread::SetName()` 修改对象名称与其他线程读取 `name_` 时的同步问题。
- IPv6 支持，以及当前 Reactor 主线之外的协程工程化与整合。

上述事项尚未解决，不因基础测试通过而视为完成；除非用户明确要求，不主动扩大当前任务处理这些问题。

## 验证记录与文档分工

- 最近一次记录的完整代码验证为 2026-10-17：测试和示例均编译成功，19 项 CTest 通过。
- EventLoopThread 测试覆盖未启动析构、初始化就绪、跨线程任务、重复启动、立即析构、循环先退出和启动期间退出，并覆盖多翻译单元与 NDEBUG。
- EventLoopThreadPool 测试覆盖零线程回退、初始化就绪、轮询与跨线程任务、析构回收、非法配置、重复及重入启动、启动失败清理和线程归属，并覆盖多翻译单元与 NDEBUG；通过回调复制异常验证部分启动后的回收，不涉及工作线程内异常传播。
- EPollPoller 测试使用真实 epoll 后端；EventLoop 独立测试使用 `tests/support/` 的 poll 后端进行故障注入，两者用途不同。
- 构建和定向验证命令统一维护在 [AGENTS.md](../AGENTS.md)，实际测试数量以 CMake/CTest 为准。
- 学习细节维护在 [learning-log/](learning-log/)；通用协作约定维护在 AGENTS.md，CLAUDE.md 仅作为入口。
- 按当前安排暂不编写 README.md。
