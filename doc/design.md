# 当前实现约定

本文记录当前 netlib 的接口、线程与生命周期契约，修改相关模块时与源码一同核对。模块状态和待办见 [progress.md](progress.md)，原理与源码阅读过程见 [学习笔记](learning-log/mymuduo.md)。

项目保持 header-only；类外函数及共享状态按需使用 C++17 inline，避免多翻译单元重复定义。CMake 接入方式见 [install.md](install.md)。

## EventLoop、Channel 与 Poller

源码：[eventloop.h](../include/netlib/eventloop.h)、[channel.h](../include/netlib/channel.h)、[poller.h](../include/netlib/poller.h)。

- 每个线程至多存在一个 EventLoop。Channel 的注册、更新与移除在所属 loop 线程执行；跨线程通过 runInLoop 或 queueInLoop 提交任务，并保证对象及目标 loop 存活。
- checkLoop 和 checkLoopInThread 是定义在 EventLoop 完整定义之后的 inline 自由函数。前者仅验证非空，空指针抛 std::invalid_argument；后者额外检查所属线程，错误线程在 NDEBUG 下仍终止。
- runInLoop 在所属线程可立即执行，queueInLoop 始终排队。quit 请求结束循环，不承诺排空全部待办任务；对象清理不能依赖循环退出后继续执行队列。
- Channel::tie 保存弱引用，在整个事件回调分发期间取得强引用保护；禁用事件后仍需移除 Channel，再销毁它。Poller 仅借用 Channel 指针，不释放对象或关闭 fd。
- EPollPoller 与 Poller 基类分离，当前默认工厂只提供 epoll；请求不支持的后端会报告错误。

## 线程封装

源码：[thread.h](../include/netlib/thread.h)、[eventloopthread.h](../include/netlib/eventloopthread.h)、[eventloopthreadpool.h](../include/netlib/eventloopthreadpool.h)。

### Thread

- 构造即启动，空回调在启动和计数前抛 std::invalid_argument；构造等待 TID 发布，不代表 EventLoop 已就绪。
- 默认名称在启动前确定，并与工作线程 TLS 名称一致；numCreated 是累计创建计数。join 状态由 std::thread 管理，不另设启动或运行标志。
- 析构回收仍可 join 的线程，不能在自身工作线程销毁 Thread。入口异常和名称并发修改问题见 progress 的暂缓事项。

### EventLoopThread 与线程池

- EventLoopThread 在 startLoop 中创建 Thread；工作线程在栈上创建 EventLoop，执行可选初始化回调后，经互斥锁与条件变量发布借用指针。循环快速退出被启动方观察到时，startLoop 抛 std::runtime_error。
- EventLoopThread 只允许启动一次，重复启动抛 std::logic_error；启动与销毁由同一控制线程串行执行。析构持锁检查 loop 并请求 quit，解锁后 join，即使循环已退出也回收线程。
- EventLoopThreadPool 的构造、配置、启动、查询与析构均在 baseLoop 线程执行。负线程数抛 std::invalid_argument；首次启动尝试后不可重试或重新配置，重复及重入启动抛 std::logic_error。
- 线程池等待各工作循环初始化完成；零工作线程时在 baseLoop 执行初始化回调，分配也回退到 baseLoop，否则仅在工作循环之间轮询。
- `startAttempted_` 在执行回调前设置，`started_` 仅在全部初始化成功后设置。控制线程中的启动异常会清理已创建线程并传播；失败后 started() 为 false，不能获取循环。工作线程内部异常仍遵循暂缓策略。
- 所有返回的 EventLoop 指针均为借用，仅在对应循环存活时有效；使用期间工作循环不得自行退出。baseLoop 必须比线程池存活更久，线程池析构仅停止并回收工作线程。

## Buffer

源码：[buffer.h](../include/netlib/buffer.h)。

- `vector<char>` 划分预留区、可读区和可写区，支持初始容量为 0 及独立复制。容量计算防止回绕，超大请求抛 std::length_error；retrieve、retrieveAsString、hasWritten 的长度约束使用 NETLIB_ASSERT，在 NDEBUG 下生效。
- retrieveAllAsString 提取全部可读数据并复位索引。append 支持外部数据及可读区自追加，长度为 0 时允许空指针；搬移或扩容前保护自追加源数据。数据搬移或重新分配会使旧 peek 指针失效。
- readFd 与 writeFd 都在系统调用前检查 savedErrno 非空；仅失败时写回立即保存的 errno。失败和 EOF 不消费原有数据，成功及零字节操作不改写错误码输出。
- readFd 使用 readv，返回 0 表示 EOF；writeFd 使用通用 write，成功时仅消费实际写出字节。重试与 fd 生命周期由调用方管理，writeFd 的 SIGPIPE 策略也由调用方负责；TcpConnection 使用 send(MSG_NOSIGNAL)。

## Socket 与 Acceptor

源码：[socket.h](../include/netlib/socket.h)、[acceptor.h](../include/netlib/acceptor.h)。

- Socket 独占传入的非负 fd，析构统一 close；外部不能再次关闭。已有 fd 的 NONBLOCK/CLOEXEC 标志由创建方设置。
- Socket::adopt 在构造 Socket 前的分配失败路径也回收 fd。将裸 fd 直接交给 `make_shared<TcpConnection>` 时，连接构造之前的分配失败仍由调用方负责；接入路径推荐先 adopt，再移动 Socket 所有权，TcpServer 已采用此方式。
- getLocalAddr 零初始化地址，getsockname 失败时抛带原始 errno 与 fd 上下文的 std::system_error。绑定与监听失败记录原始错误后 FATAL 终止；写端关闭与选项设置保持 void，失败记日志而不抛异常，选项值使用 int。
- Socket::accept 要求非空地址指针，成功返回 NONBLOCK/CLOEXEC fd；失败返回 -1、保留 errno，不修改对端地址。shutdownWrite 仅关闭内核写端，用户态缓冲排空由连接层组织。
- Acceptor 在所属线程检查 loop 后创建、配置并绑定监听 socket，reuseport 默认 false。构造不监听、不注册读事件；配置、监听、查询与析构同样要求所属线程，loop 必须比 Acceptor 存活更久，回调期间不能销毁 Acceptor。
- listen 在内核监听与 Channel 注册均成功后置位，重复调用直接返回；注册失败可重试或销毁，此时内核 socket 可能已监听。
- 当前 LT 模式每次读就绪接收一个连接。EINTR、EAGAIN/EWOULDBLOCK 直接返回，其他错误记日志；EMFILE 恢复能力见 progress 待办。
- 调用新连接回调时转交 fd 所有权，回调负责接管及异常清理；对端地址引用仅在本次调用有效。Acceptor 不在回调抛异常后无条件 close，未设置回调时直接关闭 fd。
- 析构先禁用并移除监听 Channel，再由 Socket 关闭 fd。listenAddress 在所属监听 loop（Server 的 baseLoop）线程查询实际绑定地址，支持端口 0；TcpServer::ipPort 保留构造配置字符串。

## TcpConnection

源码：[tcpConnection.h](../include/netlib/tcpConnection.h)、[Callbacks.h](../include/netlib/Callbacks.h)。

### 建立、读取与回调

- 活动连接由 shared_ptr 管理，fd 始终由内部 Socket 持有。支持裸 fd 与 `unique_ptr<Socket>` 构造；进入构造后的检查、名称复制或 Channel 分配失败由资源拥有者清理，构造前的分配窗口见 Socket 约定。
- 构造只要求 loop 非空，允许在其他线程创建；Connecting 状态尚未注册 Channel，可直接取消并析构。connectEstablished 在所属 I/O 线程设置状态、Channel tie 和读事件。
- 连接与回调别名统一在 Callbacks.h；MessageCallback 参数为 `const TcpConnectionPtr&`、`Buffer*`、`Timestamp`。回调在发布连接前或所属 I/O 线程配置，不支持并发修改；连接上线、下线等可选回调先判空。
- LT 读取遇到 EINTR、EAGAIN/EWOULDBLOCK 保留连接和读事件；其他错误使用 readFd 保存的原始 errno 记录并关闭，不用 SO_ERROR 替代该错误。正常数据及时间戳交给消息回调，EOF 按下述关闭约定处理。

### 发送

- 原始内存、string 和 Buffer 重载共用按地址与长度发送的实现，保留二进制数据。空消息不发送、不触发写完成；Buffer 指针必须非空，调用时观察到 Connected 才接管可读数据并在调用线程清空，Buffer 对象仍属于调用方。
- 所属线程直接尝试发送，跨线程复制消息并持有连接的 shared_ptr。任务执行时重新检查状态；shutdown、forceClose 或 EOF 开始关闭后拒绝新发送，已排队但尚未进入输出缓冲的请求也可能被放弃。
- 有积压时追加到输出缓冲尾部，按 I/O loop 实际执行请求的顺序发送。首发与续发均使用 send(MSG_NOSIGNAL)，EINTR 重试，部分写入和 EAGAIN/EWOULDBLOCK 保留余量并通过写事件继续；其他错误记录原始 errno 并关闭。
- 写完成表示本轮用户态输出已交给内核，不代表对端已收到。写完成及高水位通知排队执行，持有连接与入队时的回调副本；高水位默认 64 MiB，仅在从阈值以下跨到阈值及以上时通知。

### 关闭与最终清理

| 触发方式 | 输出与读取行为 |
|---|---|
| 本端 shutdown | 停止接受新发送，已有输出排空后关闭写端，继续读取对端数据 |
| 对端 EOF | 取消读事件并拒绝新发送，排空已有输出后关闭写端并通知关闭 |
| forceClose / 致命读写错误 | 不等待输出排空，进入关闭流程 |

- shutdown 用 CAS 将 Connected 转为 Disconnecting，避免覆盖 Disconnected；跨线程任务持有连接，执行时重新检查状态。shutdownInLoop 在输出为空且无写事件时最多请求一次 shutdownWrite；本端先 shutdown 后再收到 EOF 也不会重复请求。
- EOF 后停止重复读取；输出排空时先请求 FIN，再调用 handleClose，即使外部仍持有连接也能观察到 FIN。正常关闭仅保证已有输出缓冲排空，超时与强制中止边界见 progress。
- forceClose 支持 Connected 和 Disconnecting，即使在所属线程也排队，避免同步移除本批其他活跃 Channel；过期任务与重复关闭被忽略。
- handleClose 禁用并移除 Channel、设置 Disconnected，再通知下线和关闭。写完成回调仍在队列中，可能晚于这两个同步通知。
- connectDestroyed 在所属线程执行，允许取消未建立连接或重复清理；主动销毁已建立连接发出一次下线通知，不额外调用关闭回调。当前 Poller 允许重复 remove，保留两个清理入口的移除行为。
- connectDestroyed 全程持有连接，Channel tie 保护事件分发，任务强引用保护排队执行；最后一个连接引用释放时才关闭 fd。逻辑关闭不等于立即释放 fd，外部引用也不能延长所属工作 loop 的有效期。

## TcpServer

源码：[tcpServer.h](../include/netlib/tcpServer.h)。

- 构造、配置、启动与析构均在 baseLoop 线程执行。配置仅允许在首次启动尝试之前；重复及重入 start、启动后修改配置均抛 std::logic_error。start 传递初始化回调，工作线程就绪后再监听；失败回收线程池，不支持重试。
- 接入先 adopt fd，再查询地址、生成名称、构造并配置连接、入表。异常回滚未建立连接；建立任务在 I/O loop 排队并检查 Server 存活标志，注册失败清理 Channel 后通知 baseLoop 移表，后续连接仍可接入。
- connections_ 仅在 baseLoop 修改。关闭通知排队到 baseLoop，捕获 Server 存活标志及连接弱引用，确认存活后才访问 Server；移表后按值持有连接，交给所属 I/O loop 执行 connectDestroyed。
- 连接、写完成回调可为空；默认消息回调消费输入，显式清空该回调时也采用此策略。用户回调应不抛异常，工作线程回调不得同步等待正在析构的 baseLoop。
- 在事件分发之外析构：先使存活标志失效并停止接入，清理 baseLoop 连接，再向每个工作 loop 提交清理及下一轮队列确认；即使连接已移表也等待此前任务和本批回调结束，最后释放连接表、停止并回收线程池。工作 loop 不得自行退出，否则无法完成同步确认。
- Server 析构按强制清理处理，不承诺待发数据排空，业务层优雅下线由应用组织。外部引用可能延后 fd 关闭；Server 销毁后不能继续使用其工作 loop 指针，或对保留连接调用需要该 loop 的方法。
