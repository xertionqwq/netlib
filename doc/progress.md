# 项目进度

更新日期：2026-11-07。

## 当前状态

项目为 C++17、Linux、header-only 网络库，无第三方库依赖。当前沿 muduo 路线完善 Reactor；线程、连接与服务端已形成最小实现，回声示例、CMake 安装和自动回归入口已接通。

muduo 整体回顾已完成，见 [学习笔记](learning-log/mymuduo.md)。文档已按职责整理；跨端压测阶段已完成：3 Mbps 时公网 5 B ping-pong 平台约 6.2k QPS，50 Mbps 时约 29k，上限定位在客户端接入侧的小包事务率，而非 EIP 或 netlib 服务端；本机回环 70k–105k QPS，受单核 CPU 限制。后续撰写 README。协程仍为学习原型，后续单独讨论工程化与整合。

## 模块状态

| 模块 | 状态 | 当前能力与边界 |
|---|---|---|
| Noncopyable | 已实现 | 禁止资源类拷贝 |
| Logger / Timestamp | 已实现 | 同步流式日志、级别过滤、errno 保存、FATAL、项目断言与本地时间格式化 |
| CurrentThread / Thread | 基本实现完成 | Linux TID、构造即启动、线程命名及 join 回收 |
| Channel | 核心实现完成 | 事件关注与分发、时间戳传递、tie 生命周期保护 |
| Poller / EPollPoller | 核心实现完成 | 抽象接口、epoll 后端与注册状态管理；默认工厂仅支持 epoll |
| EventLoop | 基本实现完成 | 单线程事件循环、任务队列、eventfd 跨线程唤醒和退出 |
| EventLoopThread | 最小实现 | 延迟启动、初始化就绪同步、循环退出与线程回收 |
| EventLoopThreadPool | 最小实现 | 工作循环管理、零线程回退、轮询分配和启动失败清理 |
| Buffer | 最小实现 | 读写索引、自追加、容量检查及 fd 读写 |
| InetAddress | 已有实现 | IPv4 地址封装 |
| Socket | 最小实现 | 独占 fd、绑定接入、地址查询、写端关闭和常用选项 |
| Acceptor | 最小实现 | 监听与接入、fd 交接和错误分类；EMFILE 恢复待完善 |
| TcpConnection | 最小实现 | 缓冲读写、跨线程发送、EOF 后输出排空、半关闭和强制关闭 |
| TcpServer | 最小实现 | 接入分发、连接管理、回调转交和同步析构清理 |
| EchoServer 示例 | 最小示例 | 持续字节回声、端口与线程配置、READY 通知和信号停止 |
| 构建与验证 | 已接通 | CMake 安装导出、日常回归和独立使用方安装验证 |
| Fiber / Scheduler / Timer | 已有学习原型 | 尚未与 Reactor 整合，跨翻译单元和 NDEBUG 适配待审查 |
| IOManager / Hook / Stream / ByteArray | 后续方向 | 尚未实现，引入方式届时再定 |

接口、线程归属和生命周期约定集中在 [design.md](design.md)；测试覆盖与运行方式见 [testing.md](testing.md)。

## 下一步

1. 撰写 README，提供项目介绍、快速运行和文档导航。
2. 压测阶段结束；如需继续测服务端公网能力，改用云上同地域客户端，并按 [压测计划与结果](benchmark-plan.md) 收尾临时安全组规则。
3. 根据实际使用补充 Reactor 场景，逐项讨论停止、超时等收尾需求。
4. 后续再回顾协程原型，确定与网络层的整合方向。

上述路线不表示已授权直接实现全部后续模块。

## 已知限制与待办

- TcpServer 在 baseLoop 线程、事件分发之外同步析构；没有显式异步 stop，析构不保证待发数据排空。业务层优雅下线由应用组织。
- Acceptor 仅分类并记录 EMFILE；持续耗尽 fd 时可能反复触发监听事件，恢复策略待定。
- 没有连接空闲或输出排空超时。对端 EOF 后若停止读取，输出排空可能长期等待，可由应用请求 forceClose。
- 当前仅支持 IPv4，默认 Poller 工厂仅提供 epoll 后端。
- 公网小包压测受客户端接入侧限制；尚未用云上同地域客户端验证 50 Mbps 以上的服务端公网能力。

## 暂缓事项

- Thread 入口、EventLoopThread 初始化及循环异常的传播与恢复策略。
- Thread::SetName 修改名称与其他线程读取之间的同步问题。
- 协程原型的工程化审查及 Reactor 整合。

暂缓事项除非用户明确要求，不主动扩展处理。

## 最近验证

2026-11-05 的代码验证：

- 完整 Debug 构建与 32/32 项 CTest 通过，包含 NDEBUG 专项及正式回声程序子进程回归。
- 两种安装布局迁移后的四组 Debug/Release 独立使用方验证通过。
- Release 验证范围为独立使用方与回声程序专项，不代表协程原型已通过 Release 回归。

2026-11-06 首轮压测（Release + WARN 日志补丁，`--threads 1`）：

- 跨端：EIP 固定带宽 3 Mbps，JMeter 5.6.3 长连接 5 字节 ping-pong；200 并发 5,607 QPS（约 2.65 Mbps），500/1000 并发稳定在 6.0–6.3k QPS；1000 并发时服务端约 8% CPU、系统 idle 88–93%，确认瓶颈为 EIP 出向带宽，而非回显处理。
- 回环：临时 epoll 客户端与服务端共享 2 vCPU，单 IO 线程峰值 105,495 QPS（2 客户端 × 64 连接），此时 CPU 92–97% 饱和；连接数增加后回落到 6.6–7.8 万 QPS。
- 延迟、带宽实测、误差口径和后续建议见 [benchmark-plan.md](benchmark-plan.md)。

2026-11-07 第二轮压测（EIP 峰值 50 Mbps）：

- 家宽 JMeter 5 B：1000 并发 27,510.5 QPS（0 错误，p95 41 ms），2000 并发 28,682.4 QPS（8 错误，0.00%，p95 96 ms）；WSL loopclient 5 B 在 1000/2000/3000 连接下约 2.7–2.9 万 QPS，`QPS × RTT ≈ 连接数`，对应约 28–29k 事务/秒的单一队列。
- payload 扫描（2000 连接）拐点约 200 B：5/64 B 约 29.3k QPS（约 14/27 Mbps 线速），256/1024 B 降到 19.3k/5.3k QPS（约 48/45 Mbps），转入 50 Mbps 带宽限制。
- 手机热点同服务端仅约 4.6k QPS（约 9k pps 往返封顶），上传 16.7 Mbps 未饱和；同一服务端仅换客户端网络就从 29k 降到 4.6k，定位小包上限在客户端接入侧。
- 服务端 3000 连接时约 30–34% 单核 CPU、系统 idle 67–72.5%；本机回环 71.5k QPS（1000 连接），`--threads 2` 78.1k；EIP 不支持 hairpin。详细表格与安全提醒见 [benchmark-plan.md](benchmark-plan.md)。

历次排查与阶段验证保留在 Git 历史中；复跑入口与覆盖边界见 [testing.md](testing.md)。

## 文档入口

- [实现约定](design.md)：当前接口、线程和生命周期契约。
- [压测计划与结果](benchmark-plan.md)：多轮跨端与回环结果及 50 Mbps/手机热点对比。
- [安装与接入](install.md)、[构建与测试](testing.md)：使用步骤与验证入口。
- [学习笔记](learning-log/)、[协作约定](../AGENTS.md)：原理记录与仓库协作规则。
