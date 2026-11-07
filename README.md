# netlib

netlib 是一个面向 Linux 的 C++17 header-only 网络库，通过对照 muduo 学习并实现 Reactor 网络编程。项目使用 C++ 标准库与 Linux/POSIX 接口，无第三方库依赖。

当前已形成 TCP 服务端的最小实现，并提供持续回声示例、自动回归脚本及 CMake 安装导出。Fiber、Scheduler 和 Timer 仍为学习原型，尚未与 Reactor 整合；模块状态与后续路线见 [项目进度](doc/progress.md)。

## 当前能力

- **事件循环**：基于 epoll 的 LT 事件通知，由 Channel 分发回调；通过任务队列与 eventfd 支持跨线程提交任务。
- **线程模型**：每个线程一个 EventLoop，主循环接收连接，工作循环按轮询分配连接；零工作线程时由主循环处理 I/O。
- **TCP 与缓冲**：IPv4 非阻塞 socket、连接生命周期管理、Buffer 读写缓冲、跨线程发送、半关闭及对端 EOF 后排空已有输出。
- **基础设施**：同步流式日志、日志级别过滤、时间戳与项目断言；CMake target 传递 C++17 和线程依赖。

公共头文件位于 [include/netlib/](include/netlib/)，正式示例位于 [examples/](examples/)，测试位于 [tests/](tests/)。线程归属、回调和对象生命周期约定见 [设计文档](doc/design.md)。

## 项目目录

核心实现按头文件组织，均位于 include/netlib/。以下列出整体目录与模块职责，测试源码和学习笔记按目录概括，省略构建产物。

```text
netlib/
├── CMakeLists.txt                 # 构建入口、测试登记与安装导出
├── include/netlib/                # header-only 公共头文件
│   ├── tcpServer.h               # 服务端接入分发与连接管理
│   ├── acceptor.h                # 监听 socket 与新连接接收
│   ├── tcpConnection.h           # 连接状态、缓冲收发与关闭
│   ├── Callbacks.h               # 连接与消息等回调类型别名
│   ├── buffer.h                  # 输入/输出缓冲与 fd 读写
│   ├── socket.h                  # socket 资源与系统调用封装
│   ├── inetAddress.h             # IPv4 地址封装
│   ├── eventloop.h               # 事件循环、任务队列与跨线程唤醒
│   ├── channel.h                 # fd 事件关注与回调分发
│   ├── poller.h                  # I/O 多路复用抽象接口
│   ├── epollpoller.h             # epoll 后端
│   ├── defaultPoller.h           # 默认 Poller 工厂
│   ├── eventloopthread.h         # 单个工作循环的启动与回收
│   ├── eventloopthreadpool.h     # 工作循环管理与轮询分配
│   ├── thread.h                  # 线程封装
│   ├── currentThread.h           # 当前线程信息
│   ├── log.h                     # 同步流式日志与项目断言
│   ├── timestamp.h               # 时间戳与时间格式化
│   ├── noncopyable.h             # 禁止拷贝的辅助基类
│   ├── fiber.h                   # 协程学习原型
│   ├── scheduler.h               # 协程调度学习原型
│   └── timer.h                   # 定时器学习原型，尚未接入 Reactor
├── examples/
│   ├── echo_server.cpp           # 回声服务器入口
│   └── echo_server.h             # 示例配置、回声回调与信号停止
├── tests/
│   ├── test_*.cpp                # 模块专项、辅助翻译单元与进程回归
│   ├── support/netlib/defaultPoller.h  # EventLoop 测试的故障注入后端
│   └── install_consumer/         # 独立使用方安装与链接验证
├── scripts/
│   ├── test.sh                   # 日常构建与回归入口
│   └── test_install.sh           # 安装布局、迁移与使用方验证
├── cmake/
│   └── netlibConfig.cmake.in     # 安装包配置模板
├── doc/
│   ├── progress.md               # 项目状态、路线与待办
│   ├── design.md                 # 接口、线程与生命周期契约
│   ├── testing.md                # 测试入口与覆盖边界
│   ├── install.md                # 安装与外部项目接入
│   ├── benchmark-plan.md         # 压测流程、结果与统计边界
│   └── learning-log/             # muduo、协程与调度学习笔记
├── README.md                     # 项目介绍与快速入口
├── AGENTS.md                     # 通用协作与代码约定
└── CLAUDE.md                     # 读取通用约定的入口
```

## 快速运行回声服务器

需要 Linux、支持 C++17 的编译器和 CMake 3.14 或更新版本。以下命令从仓库根目录执行，只构建示例：

```sh
cmake -S . -B build/quickstart \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNETLIB_BUILD_EXAMPLES=ON \
    -DNETLIB_BUILD_TESTS=OFF
cmake --build build/quickstart --target echo_server -j 2
./build/quickstart/echo_server --port 12300 --threads 1
```

看到 `READY 0.0.0.0:12300` 后，在另一个终端用 nc 连接：

```sh
nc 127.0.0.1 12300
```

输入文本并换行，服务器会持续回显收到的字节。中文验证也使用 nc 或普通 TCP 客户端；Telnet 的控制字符与协议处理可能改变发送内容。

在服务端终端按 Ctrl+C，程序清理连接与工作线程后输出 `STOPPED` 并退出。信号停止不保证活动连接的待发数据排空。

可用参数：

- `--port N`：监听端口，默认 12300；传入 0 由系统分配临时端口，实际地址在 READY 中输出。
- `--threads N`：工作线程数，默认 3；传入 0 使用主循环处理 I/O。
- `--help`：查看帮助。

示例入口见 [echo_server.cpp](examples/echo_server.cpp)，回声回调与信号停止支持见 [echo_server.h](examples/echo_server.h)。示例在入口设置 WARN 日志级别，日志写入 stderr，READY/STOPPED 写入 stdout。

## 安装与接入

安装内容为公共头文件和 CMake 包配置。以下命令安装到仓库的 `build/prefix/`，无需系统目录写入权限：

```sh
cmake -S . -B build/install \
    -DNETLIB_BUILD_TESTS=OFF \
    -DNETLIB_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/build/prefix"
cmake --build build/install --target install
```

独立项目通过 `CMAKE_PREFIX_PATH` 指定该安装前缀，并在自己的 CMakeLists.txt 中使用：

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app LANGUAGES CXX)

find_package(netlib CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE netlib::netlib)
```

`netlib::netlib` 传递头文件路径、C++17 要求与线程依赖。完整使用方示例、安装目录迁移和 `add_subdirectory` 接入方式见 [安装文档](doc/install.md)。

## 构建与验证

仓库提供两个 Bash 脚本：

```sh
scripts/test.sh -j 2
scripts/test_install.sh -j 2
```

前者构建 Debug 测试与示例并运行 CTest，包含已有 NDEBUG 专项及正式回声程序子进程测试；后者验证安装前缀迁移和独立使用方的 Debug/Release 构建运行。

测试覆盖真实网络收发、缓冲与关闭、并发生命周期、错误路径及多翻译单元链接。定向运行、脚本选项和失败现场见 [测试文档](doc/testing.md)；现有 Release 验证范围不包含协程原型的完整回归。

## 压测记录

已在 2 vCPU / 2 GiB 云服务器上完成两轮压测，使用 Release 构建、WARN 日志及 5 B 长连接 ping-pong：3 Mbps 公网配置约 6.2k QPS，50 Mbps 配置约 29k QPS；客户端与服务端同机的回环测试约 70k–105k QPS。

这些数值反映当时的配置、客户端和网络路径；回环客户端与服务端共享两核，尚未测得独立服务端极限。测试配置、错误与延迟、统计边界及仓库外产物说明见 [压测计划与结果](doc/benchmark-plan.md)。

## 当前边界

- 网络层目前支持 IPv4，默认事件后端为 epoll；连接空闲与输出排空超时尚未实现，Acceptor 的 EMFILE 恢复策略待完善。
- TcpServer 在主循环所属线程、事件分发之外同步析构；显式异步 stop 与业务层优雅下线仍待后续讨论。
- 协程与调度相关代码保留为学习原型，工程化审查及与网络层的整合尚未完成。

更多待办与暂缓事项见 [项目进度](doc/progress.md)。

## 文档导航

| 文档 | 内容 |
|---|---|
| [项目进度](doc/progress.md) | 模块成熟度、路线与已知限制 |
| [设计文档](doc/design.md) | 接口、线程归属与生命周期契约 |
| [安装与接入](doc/install.md) | CMake 安装、包查找与源码接入 |
| [构建与测试](doc/testing.md) | 回归入口、覆盖边界与问题排查 |
| [压测记录](doc/benchmark-plan.md) | 公网及回环测试配置、结果与分析 |
| [学习笔记](doc/learning-log/) | muduo 回顾、协程与调度原理记录 |
