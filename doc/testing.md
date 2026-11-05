# 测试脚本

以下命令从仓库根目录执行。脚本依赖 Bash、CMake/CTest 与常见 Linux 命令，构建环境要求见 [安装说明](install.md)。脚本通过自身位置定位源码，因此也可从其他目录调用；`--build-dir` 的相对路径基于调用目录。

## 日常回归

```sh
scripts/test.sh
scripts/test.sh -j 2 -R '^test_buffer(_ndebug)?$'
scripts/test.sh -B build/check -j 2 --timeout 30
scripts/test.sh --help
```

[test.sh](../scripts/test.sh) 配置 Debug 构建，启用现有测试和示例，先构建再运行 CTest；默认构建目录为仓库下的 `build/`。并行数取 `CMAKE_BUILD_PARALLEL_LEVEL`，未设置时为 4，仅控制构建并行数。

`-R` 按 CTest 测试名称筛选，未匹配任何测试时返回失败。`--timeout` 设置 CTest 默认超时秒数，默认 30；现有用例自身的 `TIMEOUT` 属性优先。

同时启用测试与示例时，当前主构建登记 32 项测试，含已有 `_ndebug` 专项目标；关闭示例时为 31 项，不登记子进程测试。脚本固定使用 Debug，不将协程原型视为已通过 Release 回归；模块状态与暂缓事项见 [项目进度](progress.md)。

## 回声示例与当前专项验证

```sh
build/echo_server --help
build/echo_server --port 0 --threads 3
scripts/test.sh -R '^test_(tcpconnection|tcpserver|echo_support)(_ndebug)?$'
scripts/test.sh -R '^test_echo_process$'
```

示例默认端口为 12300、工作线程为 3，`--threads 0` 使用主循环处理连接。`--port 0` 请求临时端口；监听及工作线程初始化完成后，stdout 输出并刷新 `READY 0.0.0.0:<实际端口>`。客户端连接 127.0.0.1 和该端口即可，可使用 `nc` 进行持续回声验证。日志输出到 stderr；示例按字节回显，不解析 UTF-8 或 Telnet 协议。参数错误返回 2，`--help` 返回 0。

Ctrl+C 或 SIGTERM 经 signalfd 转为主循环事件，调用 quit，退出事件分发后清理监听、连接、工作线程及信号 Channel，最后 stdout 输出并刷新 `STOPPED`、返回 0。这是正常资源清理入口；TcpServer 析构仍按强制清理处理，不保证排空活动连接的待发数据。连接正常收到对端 EOF 时则先排空已有输出、发送 FIN，再通知关闭。

[test_echo_support.cpp](../tests/test_echo_support.cpp) 直接复用 [echo_server.h](../examples/echo_server.h)，登记 Debug/NDEBUG 两个目标，不要求启用示例构建。覆盖参数拒绝、实际临时端口及 READY 刷新、零/单/三个工作线程、持续中文回显、4 MiB 二进制慢读后半关闭、SIGINT 空闲退出及 SIGTERM 活动连接清理；并检查 fd/线程数、信号屏蔽字恢复及信号初始化失败的回收。背压测试限制实际内核发送缓冲，记录系统调用以确认完整输入、EOF 和积压后再开始读回声，未伪造成功发送。

上述支持测试在同一进程内组合示例类与 EventLoop；TcpConnection 原有专项另覆盖 EOF 排空期间强制关闭和写错误、本端先 shutdown、重复 EOF 防护与 FIN 去重。

[test_echo_process.cpp](../tests/test_echo_process.cpp) 是正式程序的子进程驱动，只在 `NETLIB_BUILD_TESTS=ON` 和 `NETLIB_BUILD_EXAMPLES=ON` 时构建及登记。构建该测试目标会先构建 echo_server，CTest 通过 `$<TARGET_FILE:echo_server>` 传递实际可执行文件路径，支持构建目录含空格。已有 scripts/test.sh 同时开启这两个选项，无需另设入口。

驱动使用 posix_spawn 启动正式 echo_server，不调用 EchoServer 类或包装服务端系统调用。每次使用端口 0，等待 stdout 的 READY 后连接回环地址；验证 --help 返回 0、四组非法参数返回 2，以及零/单/三个工作线程的实际线程数量。网络场景包含同连接 10 轮中文与 emoji、受控分批发送、2 MiB 全双工回显、8 个并发连接和 8 MiB 暂缓读取后半关闭。数据按累计字节核对，不依赖 TCP 分段；精确的内部背压观测仍由进程内支持测试负责。

每种线程配置分别验证 SIGINT 空闲停止和 SIGTERM 下 9 个活动连接的清理，要求完整 STOPPED 通知、正常返回 0、客户端收到 EOF，并使用 waitpid 回收进程。父进程在连接和收发等待期间同步消费 stdout/stderr，stdout 限 8 KiB、stderr 仅保留末尾 64 KiB，避免日志管道写满或无界占用内存。失败时输出当前场景、子进程状态和输出尾部。

启动和退出各限 3 秒、每组收发限 6 秒，并共享整套用例 20 秒的总期限。异常路径先发送 SIGTERM，最多等待 500 毫秒，再按需 SIGKILL 并 waitpid；只操作本驱动持有的子进程 PID。CTest 的 30 秒超时作为外层兜底，内部期限为清理留出余量。该测试可以单独在 Release 下构建 echo_server 与 test_echo_process 验证，不代表其他学习原型已通过 Release。

## 安装与独立使用方验证

```sh
scripts/test_install.sh -j 2
scripts/test_install.sh --keep
scripts/test_install.sh --help
```

[test_install.sh](../scripts/test_install.sh) 在 `TMPDIR`（默认 `/tmp`）下创建独占临时目录，完成以下流程：

1. 将 [install_consumer](../tests/install_consumer/) 复制到临时目录，作为独立 CMake 项目。
2. 关闭库自身的测试与示例，分别安装 `include/lib` 和 `headers/lib64` 两种目录布局。
3. 核对公共头文件内容，检查导出配置未包含原仓库、构建目录或旧安装前缀的绝对路径。
4. 移动含空格的安装前缀；标准布局通过 `CMAKE_PREFIX_PATH` 查找包，自定义布局通过 `netlib_DIR` 查找包。
5. 对每种布局分别构建并运行独立使用方的 Debug/Release 测试，共四次。

使用方只链接 `netlib::netlib`，不手动补充库的包含路径、C++17 要求或线程库。两个翻译单元共同检查头文件链接、共享 EventLoop 状态、二进制 Buffer 数据及真实工作线程任务；关键检查在 NDEBUG 下仍有效。

安装测试每项默认超时 10 秒，可用 `--timeout N` 修改；构建并行数与日常脚本一致。这些测试属于独立构建，不增加主项目登记的 CTest 数量。Release 结果只覆盖该使用方用到的 Reactor 接口。

## 失败与现场保留

- 参数错误返回 2，配置、构建或测试失败会返回非零状态；失败不会继续打印验证通过。
- 安装脚本成功后自动删除自身创建的临时目录；指定 `--keep` 时保留，失败时也会保留并打印目录及失败阶段。
- 安装脚本仅向临时前缀写入，不需要系统目录权限，也不修改现有构建目录；日常脚本会更新所选构建目录的 Debug 配置。

手动 CMake/CTest 验证命令见 [AGENTS.md](../AGENTS.md)。
