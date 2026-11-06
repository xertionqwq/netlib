# 构建与测试

以下命令从仓库根目录执行，环境要求见 [安装说明](install.md)。脚本依赖 Bash、CMake/CTest 与常见 Linux 命令，通过自身位置定位源码；`--build-dir` 的相对路径基于调用目录。

## 日常回归

```sh
scripts/test.sh
scripts/test.sh -j 2 -R '^test_buffer(_ndebug)?$'
scripts/test.sh -B build/check -j 2 --timeout 30
scripts/test.sh --help
```

[test.sh](../scripts/test.sh) 配置 Debug，启用测试与示例，先构建再运行 CTest；默认目录为 `build/`。构建并行数取 `CMAKE_BUILD_PARALLEL_LEVEL`，未设置时为 4。

`-R` 按测试名称筛选，未匹配任何测试时返回失败。`--timeout` 设置 CTest 默认超时，默认 30 秒；用例自身的 `TIMEOUT` 属性优先。

已有 `_ndebug` 专项目标会在 Debug 主构建中定义 NDEBUG。当前回归不代表协程原型已通过 Release；模块状态与暂缓事项见 [progress.md](progress.md)。测试数量和名称以 CMake 与 `ctest -N` 为准。

## 手动构建与定向测试

```sh
cmake -S . -B build -DNETLIB_BUILD_TESTS=ON -DNETLIB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
(cd build && ctest --output-on-failure --timeout 30)
```

从已配置的 `build/` 目录运行下列命令。先列出测试，再按需选择；正式回声程序的子进程测试要求同时启用测试与示例构建。

```sh
cd build
ctest -N
ctest -R '^(test_channel|test_epollpoller|test_eventloop|test_eventloopthread|test_eventloopthreadpool)(_ndebug)?$' --output-on-failure
ctest -R '^test_thread$' --output-on-failure
ctest -R '^test_log(_ndebug)?$' --output-on-failure
ctest -R '^test_net$' --output-on-failure
ctest -R '^test_buffer(_ndebug)?$' --output-on-failure
ctest -R '^test_tcpconnection(_ndebug)?$' --output-on-failure
ctest -R '^test_tcpserver(_ndebug)?$' --output-on-failure
ctest -R '^test_echo_support(_ndebug)?$' --output-on-failure
ctest -R '^test_echo_process$' --output-on-failure
ctest -R '^test_socket(_ndebug)?$' --output-on-failure
ctest -R '^test_acceptor(_ndebug)?$' --output-on-failure
```

网络模块专项覆盖真实 I/O、生命周期与错误路径，适用时包含多翻译单元和 NDEBUG。EPollPoller 测试使用真实 epoll；EventLoop 独立测试使用 `tests/support/` 的 poll 后端进行故障注入，两者用途不同。

## 回声示例与验证范围

```sh
build/echo_server --help
build/echo_server --port 0 --threads 3
scripts/test.sh -R '^test_(tcpconnection|tcpserver|echo_support)(_ndebug)?$'
scripts/test.sh -R '^test_echo_process$'
```

示例默认端口 12300、工作线程 3；`--threads 0` 使用主循环，`--port 0` 请求临时端口。监听与工作线程就绪后，stdout 刷新 `READY 0.0.0.0:<实际端口>`；用另一个终端的 nc 连接 127.0.0.1 和该端口。日志写入 stderr，`--help` 返回 0，参数错误返回 2。

Ctrl+C 或 SIGTERM 经 signalfd 请求退出主循环，在事件分发之外清理资源，最后刷新 `STOPPED` 并返回 0。信号停止进入 Server 强制析构，不保证活动连接的待发输出排空；正常对端 EOF 则排空已有输出后发送 FIN。完整约定见 [design.md](design.md)。

示例按字节回显；中文验证使用普通 TCP 客户端或 nc。Telnet 的本地控制字符和协议处理可能改变发送字节，本机普通模式及 `-8` 都曾复现中文异常。

| 专项 | 覆盖重点 |
|---|---|
| [TcpConnection](../tests/test_tcpconnection.cpp)，含 NDEBUG 与 peer 翻译单元 | 缓冲收发、错误与关闭；EOF 排空、本端先 shutdown、FIN 去重、拒绝新发送及 forceClose/EPIPE 中止 |
| [echo 支持测试](../tests/test_echo_support.cpp)，含 NDEBUG | 复用示例类；0/1/3 工作线程、中文持续回声、4 MiB 慢读半关闭、READY 刷新、信号停止、fd/线程与信号屏蔽字恢复、初始化失败清理 |
| [正式程序子进程测试](../tests/test_echo_process.cpp) | 实际 main、帮助与非法参数、实际线程数、连续中文/二进制/分批回声、2 MiB 双向收发、8 客户端并发、8 MiB 慢读半关闭、信号退出及 waitpid 回收 |

支持测试限制真实内核发送缓冲，确认短写或 EAGAIN、完整输入和 EOF 及输出积压后再读回声。子进程驱动通过 posix_spawn 启动正式 echo_server，不包装服务端系统调用；使用临时端口，等待 READY，检查 SIGINT 空闲退出与 SIGTERM 活动连接清理、STOPPED、返回码和客户端 EOF。

子进程测试只在测试与示例同时启用时登记，构建依赖及可执行路径由 CMake 传递。启动与退出各限 3 秒，每组收发限 6 秒，共享 20 秒总期限，CTest 外层限 30 秒。等待期间同步消费输出，stdout 限 8 KiB，stderr 仅保留末尾 64 KiB；失败报告场景和输出尾部，先 SIGTERM 等待最多 500 毫秒，再按需 SIGKILL 并 waitpid。驱动仅操作自己持有的子进程。

可单独以 Release 构建 `echo_server` 与 `test_echo_process` 验证；这不构成其他学习原型的 Release 回归。

## 安装与独立使用方验证

```sh
scripts/test_install.sh -j 2
scripts/test_install.sh --keep
scripts/test_install.sh --help
```

[test_install.sh](../scripts/test_install.sh) 在 `TMPDIR`（默认 `/tmp`）创建独占临时目录，复制 [独立使用方](../tests/install_consumer/)，关闭库自身测试与示例后安装。脚本核对公共头文件和导出配置，再迁移含空格的安装前缀，验证标准 `include/lib` 与自定义 `headers/lib64` 两种布局的 Debug/Release 构建运行。

标准布局用 `CMAKE_PREFIX_PATH`、自定义布局用 `netlib_DIR` 查找包。使用方仅链接 `netlib::netlib`，通过两个翻译单元验证 C++17 和线程依赖传递、共享 EventLoop 状态、二进制 Buffer 与真实工作线程任务；关键检查在 NDEBUG 下有效。这些独立测试不增加主项目 CTest 数量，Release 结果仅覆盖其所用接口。

每项使用方测试默认超时 10 秒，可用 `--timeout N` 修改；并行数规则与日常脚本一致。安装与接入命令见 [install.md](install.md)。

## 失败与现场保留

- 脚本参数错误返回 2，配置、构建或测试失败返回非零状态；CTest 测试日志位于所选构建目录的 `Testing/Temporary/`。
- 安装脚本成功后删除自身临时目录；指定 `--keep` 或遇到失败时保留，打印路径及失败阶段。
- 安装脚本仅向临时前缀写入；日常脚本会更新所选构建目录的 Debug 配置。
