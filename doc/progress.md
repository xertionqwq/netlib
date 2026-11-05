# 项目进度

更新日期：2026-11-05。

## 当前方向

当前优先沿 muduo 路线学习和完善 Reactor 网络库，逐步补齐线程封装、连接接入和服务端组织。协程相关代码已有学习原型，后续再讨论与网络层整合，不要求当前按旧的四阶段协程迁移顺序推进。

muduo 整体回顾已完成，笔记维护在 [mymuduo.md](learning-log/mymuduo.md)。当前已回到原对话，在 TcpServer 之上验证最小回声示例，继续明确回显与关闭语义；后续讨论 README 撰写并整理 Reactor 相关收尾事项。

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
| Buffer | 最小实现 | 补齐索引与容量检查、字符串提取、自追加及 fd 读写；覆盖真实管道收发、部分写入、错误交接、多翻译单元及 NDEBUG |
| InetAddress | 已有实现 | 封装 IPv4，支持从 sockaddr_in 构造，不支持 IPv6 |
| Socket | 最小实现 | 独占 fd、IPv4 绑定接入、地址查询、accept4 标志、关闭写端与常用选项；adopt 覆盖分配失败时的 fd 回收，已接入 Acceptor 与 TcpConnection |
| Acceptor | 最小实现 | Socket 管理监听 fd，显式 listen、幂等启动、线程归属及接入错误分类；明确回调交接契约，覆盖真实接入、错误路径、多翻译单元及 NDEBUG；EMFILE 恢复待完善 |
| TcpConnection | 最小实现 | Socket 管理 fd，具备读写缓冲、跨线程发送与关闭、Channel tie、本端缓冲排空后半关闭及独立强制关闭；支持拥有 Socket 的构造和未建立连接取消。对端 EOF 后停止读取和新发送，排空已有输出后发送 FIN 并完成关闭 |
| Fiber / Scheduler / Timer | 已有学习原型 | 已有单独测试和学习笔记，尚未与 Reactor 整合，跨翻译单元与 NDEBUG 适配仍需审查 |
| EventLoopThreadPool | 最小实现 | 管理工作循环、零线程回退和轮询分配；补齐参数与线程归属检查、启动失败清理，覆盖多翻译单元与 NDEBUG |
| TcpServer | 最小实现 | 完成接入、轮询分发、回调转交、移表及同步析构清理；具备一次启动、存活保护和接入失败回收，覆盖零线程/多线程、错误路径、多翻译单元及 NDEBUG |
| IOManager / Hook / Stream / ByteArray | 后续方向 | 尚未实现，是否引入及如何与 Reactor 协作届时再确定 |

Poller 的具体后端与基类已经分离，不再将 `poller.h` 描述为 epoll 的具体封装。Poller 保存 Channel 借用指针，不负责释放 Channel 或关闭其 fd。

网络层共用的连接与回调别名统一在 `include/netlib/Callbacks.h` 的 `netlib` 命名空间声明，通过前置声明引用 TcpConnection、Buffer 和 Timestamp。MessageCallback 使用 `const TcpConnectionPtr &`、`Buffer *`、`Timestamp` 三参形式；测试和示例已迁移到当前接口。echo 示例通过 EchoServer 包装 TcpServer，默认监听 12300 端口并配置三个工作线程，支持 --port、--threads 和 --help。消息回调仅负责原样回显，连接移除与清理由 TcpServer 组织。对端 EOF 后排空积压输出；启动后刷新 READY 及实际监听地址，SIGINT/SIGTERM 经 signalfd 请求主循环退出，析构完成后输出 STOPPED。进程内测试覆盖精确背压和资源回收，test_echo_process 另启动正式可执行程序，覆盖 main、参数退出码、真实输出管道、TCP 回声、信号退出及父进程超时回收，已接入现有日常回归入口。

## 已确定的线程方案

### EventLoop 公共检查函数

- `checkLoop()` 与 `checkLoopInThread()` 统一定义在 `eventloop.h` 的 `netlib` 命名空间，位于 EventLoop 类完整定义之后，均为 inline 自由函数；返回借用指针，不改变所有权。
- `checkLoop()` 仅验证指针非空，空指针抛出 std::invalid_argument，允许在其他线程验证目标 loop。
- `checkLoopInThread()` 在非空检查后调用 assertInLoopThread，错误线程在 NDEBUG 下仍会终止。Acceptor 与 TcpServer 的构造在创建其他资源之前使用此检查。
- TcpConnection 构造使用仅检查非空的版本，允许先在控制线程构造，再交给所属 I/O loop 建立连接；连接建立、Channel 注册、更新和移除在所属 loop 线程执行。loop 必须比连接及其排队任务存活更久。

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
- readFd(fd, savedErrno) 要求错误码指针非空，调用 readv 前检查；仅失败时写回立即保存的 errno，返回 0 表示 EOF，失败与 EOF 不消费已有缓冲数据。重试、事件循环调度和 fd 生命周期由调用方管理。TcpConnection 及其消息回调调用方已适配当前接口。
- writeFd(fd, savedErrno) 同样在系统调用前检查错误码指针非空；使用通用 write 写出当前可读区，成功时仅消费实际写出的字节，失败时保存原始 errno 并保留数据和索引。成功及零字节写入不修改错误码输出；EINTR/EAGAIN 的重试和 SIGPIPE 策略由调用方管理。TcpConnection 的 socket 发送仍使用 send(MSG_NOSIGNAL)，未改用此接口。

## 已确定的 Socket 约定

- 构造时接管传入 socket fd 的独占所有权，要求 fd 非负；外部不能再次关闭它，析构时统一 close。原有 fd 的 NONBLOCK/CLOEXEC 标志由创建方设置。
- Socket::adopt(fd) 返回 unique_ptr 并负责创建 Socket 之前的分配失败回收，调用后不再由外部 close。getLocalAddr(fd) 零初始化 IPv4 地址，getsockname 失败时立即保存 errno，并通过带原始错误码和 fd 上下文的 std::system_error 上报。
- 绑定和监听失败时保存 errno，输出包含操作、fd 和错误码的 FATAL 日志后终止。
- `accept()` 要求非空对端地址指针，使用初始化后的 IPv4 地址和长度调用 `accept4()`；成功返回的 fd 同时设置 NONBLOCK/CLOEXEC，由调用方接管。
- 接入失败返回 -1、保留 errno 且不修改对端地址，由调用方区分 EAGAIN、EINTR 等错误。
- 关闭写端和选项设置保持 void 接口，失败时记录操作、fd、选项名（适用时）和 errno，不抛异常；选项值使用 int。
- `shutdownWrite()` 只关闭内核写端；TcpConnection 负责等待用户态输出缓冲排空后调用。Socket 已接入 Acceptor 与 TcpConnection，fd 统一在 Socket 析构时释放。

## 已确定的 Acceptor 约定

- 构造时检查 loop 非空及所属线程，再创建、配置和绑定监听 socket，不调用 listen 或注册读事件；reuseport 默认 false。
- loop 是借用指针，必须比 Acceptor 存活更久；构造、配置、监听、查询和析构均在 loop 所属线程执行，回调执行期间不能销毁 Acceptor。
- `listen()` 在内核监听和 Channel 注册均成功后设置 listening 状态，重复调用直接返回。注册失败时状态保持 false，可重试或销毁；此时内核 socket 可能已经处于监听状态。
- 新连接回调按值接收。调用回调时转交 fd 所有权，由回调负责接管和异常清理；对端地址引用仅在本次调用期间有效。Acceptor 不在回调抛异常后无条件 close，以免关闭已转交或复用的 fd；未设置回调时直接关闭连接 fd。
- 当前采用 LT 模式，每次读就绪接收一个连接。EAGAIN/EWOULDBLOCK 和 EINTR 直接返回；其他错误记录立即保存的 errno，EMFILE 额外标记 fd 耗尽，但尚未提供耗尽恢复。
- 析构先禁用并移除监听 Channel，再由 Socket 析构关闭监听 fd。echo 示例通过 TcpServer::start 组织监听。Acceptor 与 TcpServer 的 listenAddress() 在所属 baseLoop 线程查询内核实际绑定地址，支持获取端口 0 分配的临时端口；TcpServer::ipPort() 仍保留构造时的配置字符串。

## TcpConnection 已确定的读取错误处理

- 当前采用 LT 模式，handleRead 遇到 EINTR、EAGAIN/EWOULDBLOCK 时直接返回，保留连接和读事件，等待下一次就绪通知。
- 其他读取错误直接记录 readFd 保存的 errno、连接名称和 fd，再调用 handleClose 进入关闭流程；不重新查询 SO_ERROR 来替代本次读取的错误码。
- 正常读取通过消息回调交付数据和接收时间戳。EOF 不记为读取错误，取消读事件并进入 Disconnecting，保留写事件直到已有输出排空，再发送 FIN 并进入关闭流程，避免截断回声或重复处理 EOF。连接建立时设置 Channel tie，保护整个事件分发期间的连接生命周期。

## TcpConnection 已确定的发送约定

- send 的原始内存、string 和 Buffer 重载共用按地址与长度发送的实现，保留二进制数据，空消息不发送也不触发写完成通知。Buffer 指针必须非空；调用时观察到连接已建立则接管其可读数据并在调用线程清空，Buffer 对象仍由调用方持有；已断开时不消费 Buffer。
- 所属 I/O 线程直接尝试发送，跨线程任务复制消息并持有连接的 shared_ptr，避免借用调用方的字符串、原始内存或 Buffer。排队任务实际执行时重新检查连接状态，已断开或已开始 shutdown 时放弃该请求。
- 有待发送数据时，后续消息追加到输出缓冲尾部；部分写入与 EAGAIN 均保留全部剩余数据，通过写事件继续发送，保持 I/O loop 实际执行发送请求的顺序。
- 首发与 handleWrite 续发都使用 send(MSG_NOSIGNAL)，EINTR 重试，EAGAIN/EWOULDBLOCK 保留待发送数据；其他错误记录保存的 errno 并进入关闭流程，不修改进程级 SIGPIPE 设置。
- 写完成通知表示本轮用户态输出已交给内核。写完成和高水位通知通过任务队列执行，持有连接及入队时的回调副本；高水位保持默认 64 MiB，仅在输出由阈值以下跨到阈值及以上时通知。
- shutdown、forceClose 或对端 EOF 开始关闭后不再接受新发送；此前已排队但尚未进入输出缓冲的发送也可能被放弃。当前只保证正常关闭时排空已有输出缓冲，不保证所有先前提交的发送都交给内核；forceClose 和致命读写错误仍可立即终止。

## TcpConnection 已确定的关闭与生命周期约定

- 活动连接通过 shared_ptr 管理。connectEstablished 在所属 loop 线程检查 Connecting 状态、设置 Channel tie 并启用读取；连接回调可为空，建立与下线通知均先判空。回调配置在发布连接之前或所属 loop 线程执行，不支持并发修改 std::function。
- TcpConnection 保留原始 fd 构造，同时提供 unique_ptr<Socket> 构造。进入构造后，loop 检查、名称复制或 Channel 分配失败均由拥有者回收 fd；Connecting 状态表示尚未注册 Channel，可直接取消并析构。make_shared 自身的分配发生在连接构造之前，因此接入路径先用 Socket::adopt 管理 fd，再向拥有 Socket 的重载移动所有权，覆盖这一段异常窗口。
- shutdown 使用 CAS 将 Connected 转为 Disconnecting，避免覆盖其他线程已经写入的 Disconnected；跨线程任务持有连接。shutdownInLoop 在执行时重新检查状态、写事件和输出缓冲，已断开则忽略，缓冲排空后最多请求一次关闭写端；系统调用失败仍按 Socket 约定记录日志。半关闭后保留读事件，允许对端继续回复，直到 EOF 或其他关闭条件到达。
- 对端 EOF 先关闭读事件、拒绝新发送，已有输出排空后由 shutdownInLoop 请求一次关闭写端并调用 handleClose。本端已先 shutdown 时也沿同一路径完成关闭，不重复发送 FIN；即使外部 shared_ptr 仍持有连接，客户端也能观察到 FIN。写完成回调仍排队执行，可能晚于同步的下线和关闭回调。
- forceClose 支持 Connected 和 Disconnecting 状态，即使在所属线程也通过 queueInLoop 排队，避免在本批事件分发中同步移除其他活跃 Channel。任务持有连接，执行时再次检查状态，直接进入 handleClose，不等待剩余输出缓冲发完；重复请求只产生一次下线和关闭通知。
- handleClose 禁用并移除 Channel、设置 Disconnected，再通知下线和关闭。connectDestroyed 在所属线程执行最终清理，允许清理尚未建立的连接和重复调用；主动销毁已建立连接会发出一次下线通知，不额外调用关闭回调。当前 Poller 允许重复 remove，保留现有两个入口的移除行为。
- connectDestroyed 在整个清理过程持有连接，防止下线回调释放调用方最后一个引用后继续访问已析构的 Channel。tie 保护整个事件回调分发，任务中的 shared_ptr 保护排队执行；fd 始终由 Socket 在最后一个连接引用释放时关闭。强制关闭进入逻辑关闭流程，不代表持有外部引用时立即释放 fd。

## TcpServer 已确定的线程与生命周期约定

- 构造、配置、启动与析构均在 baseLoop 所属线程执行。配置只允许在首次启动尝试之前完成；重复及重入 start、启动后修改配置均抛 std::logic_error，Debug/NDEBUG 行为一致。start 传递线程初始化回调，线程池就绪后再监听；失败后不支持重试，并回收已创建的线程池。工作线程初始化回调异常仍遵循既有 Thread/EventLoopThread 策略，未扩展暂缓事项。
- 接入后首先用 Socket::adopt 接管 fd，再读取本地地址、生成连接名、构造连接、配置回调和入表。拥有 Socket 的构造保证失败时不会泄漏或重复关闭 fd；配置、入表或排队失败时回滚未建立连接。建立任务在 ioLoop 排队执行，若 Server 已开始析构则取消；连接注册失败会清理 Channel 并通知 baseLoop 移表，后续连接仍可接入。
- connections_ 只在 baseLoop 修改。连接关闭通知先向 baseLoop 排队，只捕获 Server 存活标志及连接的弱引用；在 baseLoop 检查存活状态后才访问 Server。移表后按值持有连接，向所属 ioLoop 排队 connectDestroyed，避免借用已返回回调的栈变量。
- 连接与写完成回调可为空。消息回调默认消费输入缓冲，显式清空该回调时也采用这一策略。用户回调应不抛异常；工作线程回调不得同步等待正在析构的 baseLoop。
- 析构必须在 baseLoop 线程、事件分发之外执行。先使存活标志失效并销毁 Acceptor，再清理 baseLoop 上的连接，并向每个工作 loop 提交连接清理和下一轮队列确认任务；即使该 loop 的连接已经移表，也等待此前的清理与本批回调结束。完成确认后释放连接表引用，再停止和回收线程池。工作 loop 不得自行退出，否则无法完成同步清理。
- Server 析构按强制清理处理，不保证待发送缓冲排空；需要优雅下线时由应用先完成连接层 shutdown 和业务收尾。外部共享引用仍决定 fd 最终释放时机；Server 销毁后不能继续使用其工作 loop 借用指针，也不能再对保留的已断开连接调用需要该 loop 的方法。

## 已确定的构建与安装方式

- 保持 header-only，CMake 提供 INTERFACE target `netlib` 及别名 `netlib::netlib`；使用方可通过 add_subdirectory 接入源码，或在安装后使用 find_package(netlib CONFIG REQUIRED)。
- target 统一传递 C++17、头文件路径和 Threads::Threads；包配置通过 find_dependency(Threads) 查找使用方环境中的线程依赖。
- 安装公共头文件、netlibConfig.cmake 与 netlibTargets.cmake；目录布局遵循 GNUInstallDirs。使用相对安装目录时，导出配置引用安装前缀，不依赖原仓库或构建目录，可整体迁移安装目录。
- 当前尚未定义发布版本，不生成版本兼容性配置；安装方式与独立项目接入命令见 [install.md](install.md)。
- scripts/test.sh 提供 Debug 配置、测试/示例构建和 CTest 回归，支持构建目录、并行数、测试名称筛选与默认超时；空筛选结果返回失败。
- scripts/test_install.sh 使用独占临时目录，验证标准与自定义安装布局、前缀迁移，以及独立使用方的 Debug/Release 构建运行；成功后清理，失败或指定 --keep 时保留现场。使用方源码维护在 tests/install_consumer/，仅链接 netlib::netlib，验证多翻译单元、C++17 传递、Buffer 和工作线程任务；不增加主项目的 CTest 数量。脚本说明见 [testing.md](testing.md)。
- echo 示例已迁移到 TcpServer，当前继续验证回显与关闭行为；README 按后续安排推进。安装与脚本能力不改变协程原型的模块状态。

## 后续推进顺序

EventLoopThreadPool、Socket、Acceptor、Buffer 与 TcpConnection 的当前最小实现及专项验证已完成。TcpServer 在 2026-10-21 保存 WIP 快照后，于 2026-10-25 恢复整合并完成当前最小实现；后续路线不表示已经授权直接实现所有模块：

1. muduo 整体回顾已完成，类职责、资源生命周期、模块交互与设计原因见 [mymuduo.md](learning-log/mymuduo.md)，后续结合实际使用继续核对实现差异。
2. 当前 echo 已完成对端 EOF 排空修复、配置参数、就绪通知与信号正常退出，进程内和正式子进程回归均已接入 CTest 与 scripts/test.sh。后续结合使用补充场景；客户端手动验证使用原始 TCP 或 nc，避免 Telnet 的控制字符处理影响字节流。
3. 结合已验证的构建、安装、测试脚本与示例，讨论并撰写 README，说明当前能力、使用方法和实现边界。
4. 整理 muduo/Reactor 学习与实现的其他收尾事项，逐项确认处理范围；显式停止、超时下线及协程整合等方向仍需单独讨论，不因本次安排自动启动。

### 网络层已知待办

- TcpServer 当前采用 baseLoop 线程中的同步析构，没有显式异步 stop 或超时强制退出接口；回调不能同步等待该 baseLoop，工作循环不能自行退出。业务层优雅下线与协议收尾由应用组织。
- Acceptor 的 EMFILE 恢复策略待定，当前仅分类并记录日志，持续耗尽 fd 时可能反复触发监听事件；后续再讨论应急 fd 或暂停接入等方案。
- 调用方若仍将裸 fd 直接传给 make_shared，必须自行管理尚未进入构造函数的分配失败；推荐采用 Socket::adopt 与拥有 Socket 的连接构造，TcpServer 已使用此路径。
- TcpConnection 对端 EOF 的积压输出已改为排空后关闭；若对端停止读取，仍可能长期等待，没有排空超时，可由应用请求 forceClose。
- echo 已支持配置、READY 和 SIGINT/SIGTERM 正常析构，相关支持代码位于 examples/echo_server.h，不安装为公共 API。正式子进程回归已覆盖 main、退出码和输出管道；应用空闲超时仍未实现。信号停止会直接进入 TcpServer 析构清理，不承诺排空活动连接的业务输出。
- IPv6、连接超时和异步停止等能力尚未扩展。

## 暂缓事项

- Thread 执行入口及 EventLoopThread 初始化/循环异常的日志、传播和恢复策略。
- `Thread::SetName()` 修改对象名称与其他线程读取 `name_` 时的同步问题。
- IPv6 支持，以及当前 Reactor 主线之外的协程工程化与整合。

上述事项尚未解决，不因基础测试通过而视为完成；除非用户明确要求，不主动扩大当前任务处理这些问题。

## 验证记录与文档分工

- 2026-11-05 正式 echo_server 子进程回归：新增 [test_echo_process.cpp](../tests/test_echo_process.cpp)，通过 posix_spawn 启动正式程序，以真实 stdout/stderr 管道等待 READY、记录有界日志，并用普通 TCP 客户端核对回声。覆盖 --help 与四组非法参数退出码、0/1/3 个工作线程实际生效、同连接 10 轮中文与 emoji、受控分批、2 MiB 双向收发、8 个并发客户端、8 MiB 暂缓读取后半关闭及后续持续服务。每种线程配置分别检查 SIGINT 空闲退出、SIGTERM 清理 9 个活动连接、STOPPED、返回 0 和 waitpid 回收；未改变服务端业务实现。
- CMake 在测试与示例同时启用时登记 test_echo_process，并设置到正式 echo_server 目标的构建依赖与路径传递。通过 scripts/test.sh 完成完整构建及 32/32 项 Debug 回归；临时含空格路径的独立 Release 构建只构建这两个目标，其专项也通过，确认两者启用 NDEBUG，不扩大为协程原型 Release 验证。关闭示例时不登记该项，保留原有进程内测试。
- 驱动通过严格警告检查及 ASan/UBSan、泄漏检查。另使用临时受控程序完成 9 项检查：提前退出、缺失 READY、错误 READY、停止超时、缺失 STOPPED、错误回声、每次启动 2 MiB stderr 洪泛、不存在的可执行路径和关闭父进程 stdin；异常正确返回失败，需要时实际走到 SIGKILL 回收，各次结束无残留 PID。临时验证程序不成为项目运行依赖。驱动提供阶段截止时间与总期限，失败后先 SIGTERM、最多等 500 毫秒，再按需 SIGKILL 并 waitpid；日常入口与边界见 [testing.md](testing.md)。
- 2026-11-05 echo 第一至三项完善：在既有 TcpConnection 测试中先复现 Debug/NDEBUG 的 EOF 提前关闭，再修复为停止读取、排空输出、一次 FIN 和一次关闭通知。新增两组用例覆盖本端先 shutdown、EOF 不重复读取、拒绝新 Buffer 发送、保留外部引用时仍收到 FIN，以及排空期间 forceClose/EPIPE 的回收；专项现为 16 组。TcpServer 原测试补充内核实际临时端口查询验证。
- 新增 test_echo_support 与 test_echo_support_ndebug，复用示例专用头文件，不启动正式 echo_server 子进程。真实回环临时端口覆盖零/单/三个工作线程的持续中文回声及 4 MiB 二进制慢读半关闭；SO_SNDBUF 限为 4096，确认服务端已读到完整输入和 EOF、出现短写或 EAGAIN 且仍有输出积压后，客户端才开始接收，最终字节一致。另验证 READY 刷新、SIGINT 就绪后进入循环前停止、SIGTERM 活动连接清理、fd/线程数和信号屏蔽字恢复，以及 signalfd 创建/epoll 注册失败的错误交接和回收。
- 本轮测试及正式示例完整构建成功，31 项 CTest 全部通过；改动测试源与示例通过 -Wall -Wextra -Wpedantic -Werror 检查，TcpConnection 与 echo 支持测试通过 ASan/UBSan 和泄漏检查，文档相对链接通过核对。正式 main 的 --help 已手动检查；子进程启动、标准输出管道、退出码与超时回收尚未建立自动驱动，留待下一步讨论。
- 2026-11-04 echo 剩余场景验证：实际可执行程序通过同连接 30 轮与受控分片、2 MiB 全双工原样收发、16 个客户端各 4 轮并发、小消息立即半关闭和 500 次连接回收。另进行 15 秒有界稳定性观察，新增 582 次连接，fd 峰值 14、结束恢复基线 12，RSS 本次保持 5148 KiB；覆盖范围为本次短时观察。
- 临时受控启动器沿用当前 EchoServer 回调源码，仅将线程数配置参数化，使用临时端口、控制管道与系统调用记录；在零/单/三个工作线程下，确认 2 MiB 回显、并发、500 次连接回收及 4 MiB 慢读均正确。限制服务端 SO_SNDBUF 为 4096，慢读前约 4 MiB 输出积压，实际观察到短写及 EAGAIN，恢复读取后字节一致；输出积压时 RST 后仍能接入新连接。受控调用 loop.quit 后，四个活动客户端均收到 EOF，析构链完成且进程返回 0；这不构成正式示例已有停止接口的验证。
- 大消息立即半关闭复现：上述零/单/三个工作线程均已读取完整 4 MiB，但只回显 137564、135316、121632 字节，余量尚未交给内核就因 EOF 清理被丢弃。原始程序未调整发送缓冲配置时，8 MiB 发送后立即半关闭、接收端暂缓读取，本次也仅收到 786268 字节，后续新连接仍正常；截断长度取决于运行时缓冲状态。本轮共记录 30 项成功、4 项 EOF 截断复现及 1 项正式示例缺退出入口，业务实现保持本轮测试前版本。既有 TcpConnection/TcpServer 的 Debug/NDEBUG 四项 CTest 均通过，说明当前回归尚未要求 EOF 后输出排空；自动入口及该场景的验收语义待讨论。
- 2026-11-04 中文回声定位：在取消消息回调自动 shutdown 的当前示例上，原始 TCP 同连接连续发送“难过”“先人”、中文与 emoji、含 CR/LF 的文本，以及两个词各自逐字节发送并读取，共 7 组均保持字节一致。“难过”的 UTF-8 为 e9 9a be e8 bf 87，“先人”为 e5 85 88 e4 ba ba。通过受控伪终端和只转发字节的 TCP 中继复现本机 GNU inetutils telnet 2.7 的异常：Telnet 发出前已将“难过”中的 9a 替换为包含 IAC SUSP 的控制序列，并将“先人”中的 85 丢弃；服务端回传与实际收到的数据一致。普通模式及 -8 均复现该行为，属于客户端本地控制字符/协议处理；使用 nc 127.0.0.1 12300 后，两个词在同连接上回显一致。本轮中文测试不要求服务器按字符解析 UTF-8，网络层继续按字节传输；此前大数据、背压及应用正常退出仍需单独复测。
- 2026-11-04 echo 示例真实 TCP 验证：重新构建 echo_server，单独启动该可执行程序，在 127.0.0.1:12300 测试，运行时观察到一个主线程和三个工作线程。含 CR/LF 的文本、512 字节二进制、16 个并发小消息客户端及小消息发送后半关闭均回显一致；20 次 RST 断开与 100 次空连接后仍可正常接入，进程 fd 数从基线 12 恢复到 12。同连接第二轮发送 10 字节却收到 0 字节；将 hello world 换行分为两批、收到前批回声后再发后批，后 6 字节未回显；客户端不主动半关闭时，2 MiB 数据本次仅收到首段 14,480 字节，该数值取决于实际 TCP 分段。三项均与 onMessage 发送后立即 shutdown 的行为一致。本次使用临时客户端验证，未修改示例实现或新增 CTest 目标；测试结束通过 SIGTERM 回收自有进程，未验证应用层正常退出清理。慢速接收、大消息立即半关闭、零/单工作线程及长期资源稳定性仍待后续覆盖。
- 2026-10-26 测试脚本与独立使用方：新增 scripts/test.sh、scripts/test_install.sh 及 tests/install_consumer/，日常脚本的测试/示例构建与现有 29 项回归全部通过；从其他目录调用并使用相对构建路径、筛选 Buffer 两项测试也通过。安装脚本在含空格的临时路径中，验证标准/自定义布局迁移后的四组 Debug/Release 使用方，覆盖 CMAKE_PREFIX_PATH 与 netlib_DIR 两种查找方式、多翻译单元、C++17/NDEBUG、二进制缓冲与工作线程任务；检查成功清理、--keep、超时传递和 DESTDIR 隔离。另通过 22 项受控参数、空筛选、命令退出码与失败现场保留检查，并用真实 CTest 确认空筛选返回失败；脚本通过 bash -n，使用方源码通过严格警告语法检查。独立使用方不计入主项目 29 项 CTest，说明见 [testing.md](testing.md)。
- 2026-10-26 CMake 安装与导出验证：新增 netlib::netlib、构建/安装头文件路径区分、线程依赖传递及可迁移包配置；测试和示例构建成功，现有 29 项 CTest 全部通过。临时独立项目仅链接该 target，覆盖安装后接入与 add_subdirectory 接入、两个翻译单元共享 EventLoop 状态、二进制 Buffer 操作和真实工作线程任务；使用方从 C++11 设置自动提升到 C++17。安装的 22 个头文件与源码一致，两个 CMake 文件不含仓库或构建目录路径；迁移含空格的安装前缀后，独立项目 Debug/Release 均编译运行通过，自定义 headers/lib64 布局也验证通过。本次环境为 CMake 4.2.3、GNU C++ 15.2.0；独立项目的 Release 结果仅覆盖所用 Reactor 接口，不代表协程原型已通过 Release 验证。该阶段使用临时验证项目，后续脚本验证见本节记录。
- 2026-10-25 TcpServer 修复与验证：修正 Option 到 bool 的转换、析构日志和非标准头文件引用；完成初始化回调透传、一次启动与配置线程检查、默认消息消费。连接关闭任务移除引用捕获，并用存活标志保护排队的 Server 访问；析构先停止接入，确认工作 loop 清理及当前批次任务结束后再回收线程池。增加 Socket::adopt 与拥有 Socket 的连接构造，取消未建立连接时可直接析构；地址查询失败保存原始错误并抛出，不再返回未初始化地址。
- 新增 [test_tcpserver.cpp](../tests/test_tcpserver.cpp) 与 [peer 翻译单元](../tests/test_tcpserver_peer.cpp)，注册 Debug/NDEBUG 两个目标，包含 9 组用例。真实本机临时端口验证零线程/两工作线程二进制 echo、轮询与回调线程归属、可选回调、活跃连接析构、析构后的关闭通知，以及受控阻塞工作队列下取消尚未执行的建立任务；故障注入验证启动失败、Socket 分配/名称复制/Channel 分配/连接构造前分配失败、回调复制失败、地址查询与 epoll 注册失败后的 fd 回收和后续接入，另检查重复启动、重入与错误线程拒绝。测试关键检查不依赖标准 assert。
- 本轮测试和示例完整构建成功，29 项 CTest 全部通过；TcpServer 头文件独立、重复包含及测试源通过严格警告检查；TcpServer 专项通过 ASan/UBSan 与泄漏检测。同步析构仅在约定的线程和回调边界内验证，未扩展 Thread/EventLoopThread 既有异常策略、Acceptor EMFILE 恢复或异步停止。
- 2026-10-24 Buffer writeFd 验证：扩展现有 test_buffer.cpp 与 peer 翻译单元，覆盖可读偏移和二进制数据、完整及空写入、消费后复用、真实管道部分写入与续写顺序、EAGAIN 后恢复、EBADF，以及注入 EINTR/EIO 后的数据保留和调用方重试。新增参数前置检查用例先在 Debug/NDEBUG 下复现缺陷，再补齐与 readFd 一致的 NETLIB_ASSERT，确认空错误码指针在写入 fd 前被拒绝。两个既有 Buffer 测试目标均通过，测试和示例完整构建成功，27 项回归全部通过；测试源通过严格警告检查，Buffer 专项通过 ASan/UBSan 及泄漏检测。
- 2026-10-24 TcpConnection 独立测试重写：新增统一的 [test_tcpconnection.cpp](../tests/test_tcpconnection.cpp) 和 [peer 翻译单元](../tests/test_tcpconnection_peer.cpp)，注册 test_tcpconnection 与 test_tcpconnection_ndebug 两个目标，不恢复原先分散的临时专项文件。关键检查不依赖标准 assert；采用真实非阻塞 socketpair 和受限内核发送缓冲，仅对被测 fd 包装 readv、send、shutdown 以注入错误、短写并核对线程归属。
- 新独立测试包含 14 组用例：构造元数据、可选回调及取消建立，二进制读取和时间戳、EOF 前数据保留，读取错误分类及原始错误码日志，三个发送重载、空消息与回调副本，跨线程消息及连接持有，部分写入与发送顺序、EINTR/EAGAIN 恢复，真实 EPIPE 与续发致命错误，默认 64 MiB 高水位通知，半关闭后继续接收与缓冲排空后 FIN，排队发送及过期关闭任务，同线程/跨线程强制关闭及优雅关闭升级，回调释放最后引用与 Channel tie，以及并发关闭后状态保持终止。
- 本次仅新增独立测试及其 CMake/文档入口，未修改 TcpConnection 实现。测试和示例完整构建通过，当时 27 项 CTest 全部通过；两个测试源文件通过严格警告检查，独立测试另通过 ASan/UBSan 及泄漏检测。统一定向运行命令见 [AGENTS.md](../AGENTS.md)。
- 2026-10-24 此前的临时专项清理：按用户要求删除 TcpConnection 读取、发送、生命周期及对应 peer 共 6 个临时 C++ 测试源码，同步移除 6 个 Debug/NDEBUG CTest 目标、运行命令及旧构建产物。当时保留 25 项 CTest，test_net 与 Channel 测试继续提供基础集成验证；此前专项结果仅作为历史记录，不再提供旧专项复跑入口。清理后测试及示例构建成功，25 项回归全部通过。
- 2026-10-24 生命周期修复与完整回归：恢复可选连接回调判空，修复多字符日志字面量，补齐 shutdown 的 CAS 状态转换、过期任务检查和关闭写端去重，以及独立的排队 forceClose；connectDestroyed 在清理结束前持有连接，并支持取消尚未建立的连接。当时的生命周期临时专项覆盖 Debug、NDEBUG 和多翻译单元（源码现已移除），使用真实 socketpair、受限发送缓冲和 shutdown 调用观测，覆盖空回调、半关闭后继续收取、缓冲排空后 FIN、重复与过期关闭任务、排队发送的关闭边界、同线程/跨线程强制关闭及从优雅关闭升级、回调释放最后引用、Channel tie，以及并发 shutdown/forceClose 与最终清理。
- 清理前的测试及 echo 示例完整构建通过，31 项 CTest 全部通过；TcpConnection 头文件独立、重复包含通过严格警告检查。生命周期专项通过 ASan/UBSan 及泄漏检测；echo 示例通过 127.0.0.1:8888 上三个连续 TCP 客户端的收发验证，包含二进制消息和 EOF 后连接清理。本轮未改变发送与关闭的边界，未扩展实现 TcpServer。
- 2026-10-24 此前的 send 验证：当时的发送临时专项与读取专项合计 4 项 CTest 均通过，包含 Debug、NDEBUG 和多翻译单元（专项源码现已移除）；发送专项另通过 ASan/UBSan 检查（未开启泄漏检测）。发送测试使用真实非阻塞 socketpair、受限内核发送缓冲区及 send 的短写/错误注入，覆盖三个重载、二进制数据、空消息、调用方消息提前销毁、Buffer 即时消费、队列与通知持有连接、关闭后排队任务拒绝、部分写入和后续发送顺序、EINTR/EAGAIN 恢复、真实 EPIPE 与 MSG_NOSIGNAL、续发致命错误，以及默认 64 MiB 高水位首次跨越通知。当时共注册 29 项 CTest，旧调用方迁移尚未完成，未声称完整回归通过。
- 2026-10-22 handleRead 错误处理验证：当时的读取临时专项通过 Debug、NDEBUG 和多翻译单元验证（源码现已移除）。使用真实非阻塞 socketpair 和 epoll，通过一次性 readv 故障注入验证 EINTR、EAGAIN/EWOULDBLOCK 后继续收取、ECONNRESET/EIO/EBADF 的原始错误码日志及一次关闭通知，并验证二进制数据交付、时间戳传递和对端 EOF 前的数据保留。当时共注册 27 项 CTest，本轮执行读取专项；既有 test_net、test_channel、test_channel_ndebug 的接口迁移当时尚未完成，不能视为完整回归通过。
- 2026-10-22 EventLoop 公共检查函数迁移验证：22 项可构建 CTest 通过，新增覆盖两种检查的空指针拒绝、跨线程非空检查、错误线程拒绝、多翻译单元及 NDEBUG，并加强 Acceptor 构造在线程检查通过前不得创建 socket 的验证。EventLoop/Acceptor 头文件组合通过严格警告检查；TcpConnection 可独立包含，但仍有原有日志字面量警告。完整构建受测试尚未适配 TcpConnection 新构造与消息回调接口影响，3 项依赖测试未运行；TcpServer 独立包含另受现有 Option 枚举直接传给 Acceptor bool 参数的类型错误阻塞。
- 2026-10-21 Buffer 验证：新增 test_buffer 与 test_buffer_ndebug，覆盖自定义及零容量、二进制和字符串提取、空间搬移与扩容、自追加、真实非阻塞管道的 readv/EOF/EAGAIN/EBADF、错误参数检查先于读取、容量溢出、复制和多翻译单元；两项均通过，头文件通过独立和重复包含的严格警告检查，专项测试通过 ASan/UBSan 检查（未开启泄漏检测）。其余 20 项可构建回归也通过，合计执行 22 项 CTest。
- 当时完整构建因已有 MessageCallback 签名迁移未完成而失败；test_net、test_channel、test_channel_ndebug 未运行，未使用旧测试程序代表当前源码通过验证。当时 CTest 共注册 25 项，不能视为完整回归通过。
- 2026-10-21 TcpServer 阶段快照检查：TcpServer 头文件独立语法检查通过，空函数的未使用参数仍有警告；当时完整构建因 MessageCallback 签名与调用方不一致而失败，该阶段未运行 CTest。
- 2026-10-20 的历史完整验证：测试和示例均编译成功，23 项 CTest 通过。Callbacks.h 通过独立、重复包含及与 TcpServer/TcpConnection 组合包含的严格警告检查；此前 echo 示例于 2026-10-19 验证真实收发，使用 127.0.0.1:8888。
- Acceptor 测试使用真实 epoll 和 127.0.0.1 临时端口，覆盖构造绑定、reuseport、监听注册失败重试与幂等、左值回调、fd 和地址交接、无回调关闭、回调异常清理契约与 fd 复用、错误分类、线程归属、多翻译单元及 NDEBUG；对 socket、accept4、epoll_ctl 进行单次故障注入。头文件通过严格警告检查；EMFILE 测试验证分类和日志，不代表已实现耗尽恢复。
- Socket 测试使用 127.0.0.1 临时端口，覆盖接入地址和标志、EAGAIN/EBADF、错误日志及 errno、四种选项开关、写端关闭与读端继续接收、fd 析构回收、FATAL 与参数约束，并覆盖多翻译单元和 NDEBUG。头文件通过带 `-Wall -Wextra -Wpedantic -Werror` 的独立包含检查。
- EventLoopThread 测试覆盖未启动析构、初始化就绪、跨线程任务、重复启动、立即析构、循环先退出和启动期间退出，并覆盖多翻译单元与 NDEBUG。
- EventLoopThreadPool 测试覆盖零线程回退、初始化就绪、轮询与跨线程任务、析构回收、非法配置、重复及重入启动、启动失败清理和线程归属，并覆盖多翻译单元与 NDEBUG；通过回调复制异常验证部分启动后的回收，不涉及工作线程内异常传播。
- EPollPoller 测试使用真实 epoll 后端；EventLoop 独立测试使用 `tests/support/` 的 poll 后端进行故障注入，两者用途不同。
- 构建和定向验证命令统一维护在 [AGENTS.md](../AGENTS.md)，实际测试数量以 CMake/CTest 为准。
- 学习细节维护在 [learning-log/](learning-log/)；通用协作约定维护在 AGENTS.md，CLAUDE.md 仅作为入口。
- 安装说明维护在 [install.md](install.md)，脚本入口维护在 [testing.md](testing.md)；README 待后续推进。
