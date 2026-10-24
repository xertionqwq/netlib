# 项目进度

更新日期：2026-10-24。

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
| Socket | 最小实现 | 独占 fd、IPv4 绑定接入、accept4 标志、关闭写端与常用选项；覆盖错误路径、多翻译单元及 NDEBUG，已接入 Acceptor 与 TcpConnection |
| Acceptor | 最小实现 | Socket 管理监听 fd，显式 listen、幂等启动、线程归属及接入错误分类；明确回调交接契约，覆盖真实接入、错误路径、多翻译单元及 NDEBUG；EMFILE 恢复待完善 |
| TcpConnection | 最小实现 | Socket 管理 fd，具备读写缓冲、跨线程发送与关闭、Channel tie、缓冲排空后半关闭及独立强制关闭；独立测试覆盖错误路径、缓冲续发、通知与并发生命周期，包含 Debug/NDEBUG 和多翻译单元；构造异常时的 fd 交接与 TcpServer 整合待完善 |
| Fiber / Scheduler / Timer | 已有学习原型 | 已有单独测试和学习笔记，尚未与 Reactor 整合，跨翻译单元与 NDEBUG 适配仍需审查 |
| EventLoopThreadPool | 最小实现 | 管理工作循环、零线程回退和轮询分配；补齐参数与线程归属检查、启动失败清理，覆盖多翻译单元与 NDEBUG |
| TcpServer | 已有骨架（暂缓） | 已有接口、成员、构造和初步启动逻辑；连接接入、移除及析构清理仍为空，尚不构成最小实现 |
| IOManager / Hook / Stream / ByteArray | 后续方向 | 尚未实现，是否引入及如何与 Reactor 协作届时再确定 |

Poller 的具体后端与基类已经分离，不再将 `poller.h` 描述为 epoll 的具体封装。Poller 保存 Channel 借用指针，不负责释放 Channel 或关闭其 fd。

网络层共用的连接与回调别名统一在 `include/netlib/Callbacks.h` 的 `netlib` 命名空间声明，通过前置声明引用 TcpConnection、Buffer 和 Timestamp。MessageCallback 使用 `const TcpConnectionPtr &`、`Buffer *`、`Timestamp` 三参形式；测试和 echo 示例已迁移到当前构造函数及回调接口，测试和示例的完整构建已恢复。echo 示例在 EOF 后排队移除连接，释放最后一个引用后由 Socket 关闭 fd。

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

## 已确定的 Socket 约定

- 构造时接管传入 socket fd 的独占所有权，要求 fd 非负；外部不能再次关闭它，析构时统一 close。原有 fd 的 NONBLOCK/CLOEXEC 标志由创建方设置。
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
- 析构先禁用并移除监听 Channel，再由 Socket 析构关闭监听 fd。echo 示例在设置回调后显式调用 listen。

## TcpConnection 已确定的读取错误处理

- 当前采用 LT 模式，handleRead 遇到 EINTR、EAGAIN/EWOULDBLOCK 时直接返回，保留连接和读事件，等待下一次就绪通知。
- 其他读取错误直接记录 readFd 保存的 errno、连接名称和 fd，再调用 handleClose 进入关闭流程；不重新查询 SO_ERROR 来替代本次读取的错误码。
- 正常读取通过消息回调交付数据和接收时间戳，EOF 进入关闭流程且不记为读取错误。连接建立时设置 Channel tie，保护整个事件分发期间的连接生命周期。

## TcpConnection 已确定的发送约定

- send 的原始内存、string 和 Buffer 重载共用按地址与长度发送的实现，保留二进制数据，空消息不发送也不触发写完成通知。Buffer 指针必须非空；调用时观察到连接已建立则接管其可读数据并在调用线程清空，Buffer 对象仍由调用方持有；已断开时不消费 Buffer。
- 所属 I/O 线程直接尝试发送，跨线程任务复制消息并持有连接的 shared_ptr，避免借用调用方的字符串、原始内存或 Buffer。排队任务实际执行时重新检查连接状态，已断开或已开始 shutdown 时放弃该请求。
- 有待发送数据时，后续消息追加到输出缓冲尾部；部分写入与 EAGAIN 均保留全部剩余数据，通过写事件继续发送，保持 I/O loop 实际执行发送请求的顺序。
- 首发与 handleWrite 续发都使用 send(MSG_NOSIGNAL)，EINTR 重试，EAGAIN/EWOULDBLOCK 保留待发送数据；其他错误记录保存的 errno 并进入关闭流程，不修改进程级 SIGPIPE 设置。
- 写完成通知表示本轮用户态输出已交给内核。写完成和高水位通知通过任务队列执行，持有连接及入队时的回调副本；高水位保持默认 64 MiB，仅在输出由阈值以下跨到阈值及以上时通知。
- shutdown 或 forceClose 开始后不再接受新发送；此前已排队但尚未进入输出缓冲的发送也可能被放弃。当前只保证优雅关闭时排空已有输出缓冲，不保证所有先前提交的发送都交给内核；这一边界已通过阶段验证核对。

## TcpConnection 已确定的关闭与生命周期约定

- 连接通过 shared_ptr 管理。connectEstablished 在所属 loop 线程检查 Connecting 状态、设置 Channel tie 并启用读取；连接回调可为空，建立与下线通知均先判空。回调配置在发布连接之前或所属 loop 线程执行，不支持并发修改 std::function。
- shutdown 使用 CAS 将 Connected 转为 Disconnecting，避免覆盖其他线程已经写入的 Disconnected；跨线程任务持有连接。shutdownInLoop 在执行时重新检查状态、写事件和输出缓冲，已断开则忽略，缓冲排空后最多请求一次关闭写端；系统调用失败仍按 Socket 约定记录日志。半关闭后保留读事件，允许对端继续回复，直到 EOF 或其他关闭条件到达。
- forceClose 支持 Connected 和 Disconnecting 状态，即使在所属线程也通过 queueInLoop 排队，避免在本批事件分发中同步移除其他活跃 Channel。任务持有连接，执行时再次检查状态，直接进入 handleClose，不等待剩余输出缓冲发完；重复请求只产生一次下线和关闭通知。
- handleClose 禁用并移除 Channel、设置 Disconnected，再通知下线和关闭。connectDestroyed 在所属线程执行最终清理，允许清理尚未建立的连接和重复调用；主动销毁已建立连接会发出一次下线通知，不额外调用关闭回调。当前 Poller 允许重复 remove，保留现有两个入口的移除行为。
- connectDestroyed 在整个清理过程持有连接，防止下线回调释放调用方最后一个引用后继续访问已析构的 Channel。tie 保护整个事件回调分发，任务中的 shared_ptr 保护排队执行；fd 始终由 Socket 在最后一个连接引用释放时关闭。强制关闭进入逻辑关闭流程，不代表持有外部引用时立即释放 fd。

## 后续推进顺序

EventLoopThreadPool、Socket 和 Acceptor 的当前最小实现与验证已完成。2026-10-21 决定将 TcpServer 当前进度保存为 WIP 阶段快照并暂缓，后续按以下顺序学习和完善；不表示已经授权直接实现所有后续模块：

1. Buffer 当前最小实现与专项验证已完成；后续按 TcpConnection 集成中的实际需求继续补充接口。
2. TcpConnection 当前读写、关闭及调用方迁移的最小实现与验证已完成；后续明确构造失败时的 fd 交接责任，并在 TcpServer 整合中核对生命周期契约。
3. 回到 TcpServer 整合完善：组织 Acceptor、线程池和连接集合，完成连接创建、移除、退出清理及线程归属约束。
4. 补充端到端网络验证，再评估协程原型的复习、修正与整合。

### 网络层已知待办

- TcpServer 的 newConnection、removeConnection、removeConnectionInLoop 和析构函数体仍为空。newConnection 尚未接管或关闭 Acceptor 交付的 fd，当前不能用于实际连接接入；启动时也尚未传递已配置的线程初始化回调。阶段提交仅保存学习进度，不代表可用服务端或通过完整构建的基线。
- Acceptor 的 EMFILE 恢复策略待定，当前仅分类并记录日志，持续耗尽 fd 时可能反复触发监听事件；后续再讨论应急 fd 或暂停接入等方案。
- TcpConnection 构造在 Socket 接管 fd 之前仍可能因空 loop、名称复制或内存分配失败而抛异常，原始 fd 的异常交接契约尚未统一；后续结合 TcpServer 的接入路径明确，当前不声称构造过程已全面实现异常安全。
- TcpServer 尚未纳入当前测试和示例构建的实例化路径；现有测试和示例的构建及回归通过不表示 TcpServer 已可用，其 Option 到 Acceptor bool 参数的转换和连接管理仍待整合。

## 暂缓事项

- Thread 执行入口及 EventLoopThread 初始化/循环异常的日志、传播和恢复策略。
- `Thread::SetName()` 修改对象名称与其他线程读取 `name_` 时的同步问题。
- IPv6 支持，以及当前 Reactor 主线之外的协程工程化与整合。

上述事项尚未解决，不因基础测试通过而视为完成；除非用户明确要求，不主动扩大当前任务处理这些问题。

## 验证记录与文档分工

- 2026-10-24 TcpConnection 独立测试重写：新增统一的 [test_tcpconnection.cpp](../tests/test_tcpconnection.cpp) 和 [peer 翻译单元](../tests/test_tcpconnection_peer.cpp)，注册 test_tcpconnection 与 test_tcpconnection_ndebug 两个目标，不恢复原先分散的临时专项文件。关键检查不依赖标准 assert；采用真实非阻塞 socketpair 和受限内核发送缓冲，仅对被测 fd 包装 readv、send、shutdown 以注入错误、短写并核对线程归属。
- 新独立测试包含 14 组用例：构造元数据、可选回调及取消建立，二进制读取和时间戳、EOF 前数据保留，读取错误分类及原始错误码日志，三个发送重载、空消息与回调副本，跨线程消息及连接持有，部分写入与发送顺序、EINTR/EAGAIN 恢复，真实 EPIPE 与续发致命错误，默认 64 MiB 高水位通知，半关闭后继续接收与缓冲排空后 FIN，排队发送及过期关闭任务，同线程/跨线程强制关闭及优雅关闭升级，回调释放最后引用与 Channel tie，以及并发关闭后状态保持终止。
- 本次仅新增独立测试及其 CMake/文档入口，未修改 TcpConnection 实现。测试和示例完整构建通过，当前 27 项 CTest 全部通过；两个测试源文件通过严格警告检查，独立测试另通过 ASan/UBSan 及泄漏检测。统一定向运行命令见 [AGENTS.md](../AGENTS.md)。
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
- 按当前安排暂不编写 README.md。
