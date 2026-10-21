# 项目进度

更新日期：2026-10-21。

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
| Buffer | 最小实现 | 补齐索引与容量检查、字符串提取、自追加和 readv 错误交接；覆盖真实管道读取、多翻译单元及 NDEBUG |
| InetAddress | 已有实现 | 当前只封装 IPv4，不支持 IPv6 |
| Socket | 最小实现 | 独占 fd、IPv4 绑定接入、accept4 标志、关闭写端与常用选项；覆盖错误路径、多翻译单元及 NDEBUG，已接入 Acceptor，TcpConnection 待接入 |
| Acceptor | 最小实现 | Socket 管理监听 fd，显式 listen、幂等启动、线程归属及接入错误分类；明确回调交接契约，覆盖真实接入、错误路径、多翻译单元及 NDEBUG；EMFILE 恢复待完善 |
| TcpConnection | 待完善 | 已有读写缓冲、事件回调和关闭流程，跨线程接口与连接生命周期仍需补齐 |
| Fiber / Scheduler / Timer | 已有学习原型 | 已有单独测试和学习笔记，尚未与 Reactor 整合，跨翻译单元与 NDEBUG 适配仍需审查 |
| EventLoopThreadPool | 最小实现 | 管理工作循环、零线程回退和轮询分配；补齐参数与线程归属检查、启动失败清理，覆盖多翻译单元与 NDEBUG |
| TcpServer | 已有骨架（暂缓） | 已有接口、成员、构造和初步启动逻辑；连接接入、移除及析构清理仍为空，尚不构成最小实现 |
| IOManager / Hook / Stream / ByteArray | 后续方向 | 尚未实现，是否引入及如何与 Reactor 协作届时再确定 |

Poller 的具体后端与基类已经分离，不再将 `poller.h` 描述为 epoll 的具体封装。Poller 保存 Channel 借用指针，不负责释放 Channel 或关闭其 fd。

网络层共用的 `TcpConnectionPtr`、`ConnectionCallback`、`MessageCallback`、`CloseCallback` 和 `WriteCompleteCallback` 统一在 `include/netlib/Callbacks.h` 的 `netlib` 命名空间声明，通过前置声明引用 TcpConnection、Buffer 和 Timestamp。当前草稿中的 MessageCallback 已改为 `const TcpConnection &`、`Buffer *`、`Timestamp` 三参形式，但 TcpConnection、测试和示例仍使用原来的 shared_ptr 与 Buffer 引用两参形式，因此当前完整构建失败；最终回调接口及调用方迁移留到完善 TcpConnection 时统一处理。

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

## 已确定的 Buffer 约定

- 使用 `vector<char>` 保存数据，保留预留区、可读区和可写区；支持自定义初始容量（含 0）及独立复制。常量使用 C++17 constexpr，保持 header-only。
- retrieve、retrieveAsString 和 hasWritten 的长度约束使用 NETLIB_ASSERT，在 NDEBUG 下同样生效；retrieveAllAsString 返回全部可读数据并复位索引。构造与扩容的容量计算避免整数回绕，超大请求抛出 std::length_error。
- append 支持外部数据和当前可读区的自追加，长度为 0 时允许空指针；自追加需要搬移或扩容时先保存源数据，避免指针失效。原有 peek 指针在数据搬移或重新分配后可能失效。
- readFd(fd, savedErrno) 要求错误码指针非空，调用 readv 前检查；仅失败时写回立即保存的 errno，返回 0 表示 EOF，失败与 EOF 不消费已有缓冲数据。重试、事件循环调度和 fd 生命周期由调用方管理。TcpConnection 已适配该错误码接口，其 MessageCallback 迁移仍待后续处理。

## 已确定的 Socket 约定

- 构造时接管传入 socket fd 的独占所有权，要求 fd 非负；外部不能再次关闭它，析构时统一 close。原有 fd 的 NONBLOCK/CLOEXEC 标志由创建方设置。
- 绑定和监听失败时保存 errno，输出包含操作、fd 和错误码的 FATAL 日志后终止。
- `accept()` 要求非空对端地址指针，使用初始化后的 IPv4 地址和长度调用 `accept4()`；成功返回的 fd 同时设置 NONBLOCK/CLOEXEC，由调用方接管。
- 接入失败返回 -1、保留 errno 且不修改对端地址，由调用方区分 EAGAIN、EINTR 等错误。
- 关闭写端和选项设置保持 void 接口，失败时记录操作、fd、选项名（适用时）和 errno，不抛异常；选项值使用 int。
- `shutdownWrite()` 只关闭内核写端；连接层仍需负责用户态输出缓冲排空的时机。Socket 已接入 Acceptor，TcpConnection 的接入与生命周期待办仍需继续处理。

## 已确定的 Acceptor 约定

- 构造时检查 loop 非空及所属线程，再创建、配置和绑定监听 socket，不调用 listen 或注册读事件；reuseport 默认 false。
- loop 是借用指针，必须比 Acceptor 存活更久；构造、配置、监听、查询和析构均在 loop 所属线程执行，回调执行期间不能销毁 Acceptor。
- `listen()` 在内核监听和 Channel 注册均成功后设置 listening 状态，重复调用直接返回。注册失败时状态保持 false，可重试或销毁；此时内核 socket 可能已经处于监听状态。
- 新连接回调按值接收。调用回调时转交 fd 所有权，由回调负责接管和异常清理；对端地址引用仅在本次调用期间有效。Acceptor 不在回调抛异常后无条件 close，以免关闭已转交或复用的 fd；未设置回调时直接关闭连接 fd。
- 当前采用 LT 模式，每次读就绪接收一个连接。EAGAIN/EWOULDBLOCK 和 EINTR 直接返回；其他错误记录立即保存的 errno，EMFILE 额外标记 fd 耗尽，但尚未提供耗尽恢复。
- 析构先禁用并移除监听 Channel，再由 Socket 析构关闭监听 fd。echo 示例在设置回调后显式调用 listen。

## 后续推进顺序

EventLoopThreadPool、Socket 和 Acceptor 的当前最小实现与验证已完成。2026-10-21 决定将 TcpServer 当前进度保存为 WIP 阶段快照并暂缓，后续按以下顺序学习和完善；不表示已经授权直接实现所有后续模块：

1. Buffer 当前最小实现与专项验证已完成；后续按 TcpConnection 集成中的实际需求继续补充接口。
2. 完善 TcpConnection：统一回调接口，接入 Socket 与 Channel tie，补齐跨线程发送与关闭，确保输出缓冲排空后再关闭写端，并明确构造失败时的 fd 责任。
3. 回到 TcpServer 整合完善：组织 Acceptor、线程池和连接集合，完成连接创建、移除、退出清理及线程归属约束。
4. 补充端到端网络验证，再评估协程原型的复习、修正与整合。

### 网络层已知待办

- TcpServer 的 newConnection、removeConnection、removeConnectionInLoop 和析构函数体仍为空。newConnection 尚未接管或关闭 Acceptor 交付的 fd，当前不能用于实际连接接入；启动时也尚未传递已配置的线程初始化回调。阶段提交仅保存学习进度，不代表可用服务端或通过完整构建的基线。
- Acceptor 的 EMFILE 恢复策略待定，当前仅分类并记录日志，持续耗尽 fd 时可能反复触发监听事件；后续再讨论应急 fd 或暂停接入等方案。
- TcpConnection 尚未在连接建立时设置 Channel tie；跨线程 `send()` 和 `shutdown()` 分支仍有 TODO。
- TcpConnection 的 shutdown 时机、状态访问和销毁所属线程需要一并审查，避免输出未排空就关闭写端。

## 暂缓事项

- Thread 执行入口及 EventLoopThread 初始化/循环异常的日志、传播和恢复策略。
- `Thread::SetName()` 修改对象名称与其他线程读取 `name_` 时的同步问题。
- IPv6 支持，以及当前 Reactor 主线之外的协程工程化与整合。

上述事项尚未解决，不因基础测试通过而视为完成；除非用户明确要求，不主动扩大当前任务处理这些问题。

## 验证记录与文档分工

- 2026-10-21 Buffer 验证：新增 test_buffer 与 test_buffer_ndebug，覆盖自定义及零容量、二进制和字符串提取、空间搬移与扩容、自追加、真实非阻塞管道的 readv/EOF/EAGAIN/EBADF、错误参数检查先于读取、容量溢出、复制和多翻译单元；两项均通过，头文件通过独立和重复包含的严格警告检查，专项测试通过 ASan/UBSan 检查（未开启泄漏检测）。其余 20 项可构建回归也通过，合计执行 22 项 CTest。
- 本轮完整构建仍因已有 MessageCallback 签名迁移未完成而失败；test_net、test_channel、test_channel_ndebug 未运行，未使用旧测试程序代表当前源码通过验证。当前 CTest 共注册 25 项，不能视为完整回归通过。
- 2026-10-21 TcpServer 阶段快照检查：TcpServer 头文件独立语法检查通过，空函数的未使用参数仍有警告；当时完整构建因 MessageCallback 签名与调用方不一致而失败，该阶段未运行 CTest。
- 最近一次记录的完整代码验证为 2026-10-20：测试和示例均编译成功，23 项 CTest 通过。Callbacks.h 通过独立、重复包含及与 TcpServer/TcpConnection 组合包含的严格警告检查；最近一次 echo 示例真实收发验证为 2026-10-19，使用 127.0.0.1:8888。
- Acceptor 测试使用真实 epoll 和 127.0.0.1 临时端口，覆盖构造绑定、reuseport、监听注册失败重试与幂等、左值回调、fd 和地址交接、无回调关闭、回调异常清理契约与 fd 复用、错误分类、线程归属、多翻译单元及 NDEBUG；对 socket、accept4、epoll_ctl 进行单次故障注入。头文件通过严格警告检查；EMFILE 测试验证分类和日志，不代表已实现耗尽恢复。
- Socket 测试使用 127.0.0.1 临时端口，覆盖接入地址和标志、EAGAIN/EBADF、错误日志及 errno、四种选项开关、写端关闭与读端继续接收、fd 析构回收、FATAL 与参数约束，并覆盖多翻译单元和 NDEBUG。头文件通过带 `-Wall -Wextra -Wpedantic -Werror` 的独立包含检查。
- EventLoopThread 测试覆盖未启动析构、初始化就绪、跨线程任务、重复启动、立即析构、循环先退出和启动期间退出，并覆盖多翻译单元与 NDEBUG。
- EventLoopThreadPool 测试覆盖零线程回退、初始化就绪、轮询与跨线程任务、析构回收、非法配置、重复及重入启动、启动失败清理和线程归属，并覆盖多翻译单元与 NDEBUG；通过回调复制异常验证部分启动后的回收，不涉及工作线程内异常传播。
- EPollPoller 测试使用真实 epoll 后端；EventLoop 独立测试使用 `tests/support/` 的 poll 后端进行故障注入，两者用途不同。
- 构建和定向验证命令统一维护在 [AGENTS.md](../AGENTS.md)，实际测试数量以 CMake/CTest 为准。
- 学习细节维护在 [learning-log/](learning-log/)；通用协作约定维护在 AGENTS.md，CLAUDE.md 仅作为入口。
- 按当前安排暂不编写 README.md。
