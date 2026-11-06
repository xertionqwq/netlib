# 临时回声压测计划

编写日期：2026-11-06。本文用于本轮云服务器与本机之间的部署交接，环境准备和性能测试均待实际执行，当前没有 QPS 结果。完成后再提炼需要长期保留的流程与结论。

目标环境为 2 核、2 GB 云服务器运行 echo_server，本机运行 JMeter，经公网连接 TCP 12300。先建立可重复的性能基线，记录完整回声事务吞吐、延迟、错误率及资源占用。功能回归见 [testing.md](testing.md)，连接关闭边界见 [design.md](design.md)。

## 执行顺序与分工

| 阶段 | 执行位置 | 完成条件 |
|---|---|---|
| A：服务器准备 | 云服务器 | 同步代码、检查依赖、通过回归、构建 Release、完成回环收发与停止验证 |
| B：客户端准备 | 本机 | 安装 JDK/JMeter、创建测试计划、确认实际公网出口 IPv4 |
| C：连接放行 | 云控制台与服务器、本机 | 仅对本机出口地址放行 TCP 12300，本机确认可持续回声 |
| D：基线测试 | 两端配合 | 逐组压测、同步监控、保存配置及结果 |

服务器先完成 A 并返回交接信息；本机环境和出口地址确认后再进行 C。JMeter 运行在本机，云服务器只运行被测程序与监控工具。

## A：服务器准备

### 1. 同步与检查环境

Shell 示例使用 Bash。仓库相关命令从仓库根目录执行；同步前先保留服务器已有修改，后续记录的日志级别改动也需要随版本一同保留。

```sh
cat /etc/os-release
lscpu
free -h
ulimit -n
```

以下安装命令适用于 Ubuntu/Debian；其他发行版使用对应包管理器。编译器需支持 C++17，CMake 至少为 3.14。

```sh
sudo apt update
sudo apt install -y build-essential cmake git sysstat netcat-openbsd iproute2

c++ --version
cmake --version
```

依赖就绪后同步代码：

```sh
git status --short
git pull --ff-only
git rev-parse HEAD
```

另从云控制台确认公网出网带宽上限，随测试记录保存。第一轮保持现有内核网络参数，避免同时改变实现和系统配置。

### 2. 功能回归与压测日志设置

先按当前版本运行功能回归，确认服务器环境正常：

```sh
scripts/test.sh -j 2
```

正式压测前，在 [examples/echo_server.cpp](../examples/echo_server.cpp) 的 main 开头、创建任何 EventLoop 或工作线程之前，插入：

```cpp
netlib::Logger::setLevel(netlib::LogLevel::WARN);
```

这是服务器压测版本需要执行的一处示例源码调整；本次文档提交没有修改该源码，也没有新增日志参数。当前默认 DEBUG，Channel 和 epoll 路径还有频繁的 INFO 日志；重定向到 /dev/null 仍会发生格式化和加锁。只在示例入口设置级别，保留库的默认配置。WARN 不影响 stdout 的 READY/STOPPED 通知。

### 3. Release 构建与版本记录

```sh
cmake -S . -B build/bench \
    -DCMAKE_BUILD_TYPE=Release \
    -DNETLIB_BUILD_TESTS=OFF \
    -DNETLIB_BUILD_EXAMPLES=ON

cmake --build build/bench --target echo_server -j 2
build/bench/echo_server --help

mkdir -p build/bench/run
git rev-parse HEAD > build/bench/run/source.commit
git diff -- examples/echo_server.cpp > build/bench/run/perf-logging.patch
```

这里只构建 Release 示例，功能验证使用上一步的 Debug 回归。所有临时构建、日志和本地补丁记录放在已忽略的 build/ 下。结果必须同时记录 commit 和源码差异，不能只凭 commit 将 WARN 版本当作原始源码。

### 4. 启动与回环验证

先检查端口，同一端口只启动一个测试实例；若已有进程监听，先核对其用途。

```sh
ss -lntp 'sport = :12300'
```

第一轮配置一个工作线程；之后分别对比 0、1、2 个工作线程。每次切换先停止本次实例，并另存上一组日志。

```sh
bench_workers=1
nohup ./build/bench/echo_server --port 12300 --threads "$bench_workers" \
    > build/bench/run/echo.stdout 2> build/bench/run/echo.stderr &
bench_server_pid=$!
printf '%s\n' "$bench_server_pid" > build/bench/run/echo.pid

cat build/bench/run/echo.stdout
ps -p "$bench_server_pid" -o pid,comm,args
ss -lntp 'sport = :12300'
```

等待 stdout 出现 READY 0.0.0.0:12300，再进行回环收发。若未出现，检查 stderr 和进程状态。

```sh
(
    set -o pipefail
    printf 'ping\n' | timeout 5s nc -N 127.0.0.1 12300 | od -An -tx1
)
```

预期回声为 `70 69 6e 67 0a`，命令在超时前正常结束且退出码为 0。-N 在 stdin 结束后关闭客户端写端，用于这次小消息半关闭验证；正式 JMeter 基线使用持续连接。这里的 nc 命令适用于上文安装的 netcat-openbsd。

停止时使用本次记录的 PID，核对进程后发送 SIGTERM：

```sh
bench_server_pid=$(cat build/bench/run/echo.pid)
ps -p "$bench_server_pid" -o pid,comm,args
kill -TERM "$bench_server_pid"
```

确认进程退出、stdout 最后出现 STOPPED、12300 不再监听。Server 析构属于强制清理，不承诺停止时排空活动连接的输出；压测应先让客户端任务结束，再停止服务。

### 5. 服务器交接信息

完成 A 后，将以下信息返回对话，供本机准备和后续连接使用：

- 发行版、CPU/内存、编译器/CMake 版本、源码 commit 与 WARN 本地差异。
- Debug 回归结果、Release 构建结果、实际启动参数。
- READY、回环收发结果，以及 SIGTERM 后的 STOPPED 和退出情况。
- 云服务器公网 IPv4、公网出网带宽、现有主机防火墙类型与端口规则状态。

实际 IP 与运行结果在交接时提供，不将凭据或机器专属配置提交到仓库。

## B：本机 JMeter 准备

本机系统与 JMeter 版本待确认，安装时核对所选发行版的 Java 要求，可优先使用兼容的 JDK 17。先在 GUI 中配置并以单线程调试；正式负载使用命令行，并关闭查看结果树等详细监听器。[安装与 CLI 说明](https://jmeter.apache.org/usermanual/get-started.html#non_gui)

测试计划尚未生成，后续在本机保存为 echo.jmx。线程组设置线程数 `${__P(users,10)}`、Ramp-up `${__P(ramp,10)}` 秒、持续时间 `${__P(duration,120)}` 秒，循环选择永远并启用持续时间。这些参数通过 -J 传入。[属性函数说明](https://jmeter.apache.org/usermanual/functions.html#__P)

线程组内添加 TCP Sampler，初始配置如下：

| 项目 | 配置 |
|---|---|
| 处理类 | TCPClientImpl |
| 服务器 | `${__P(host,127.0.0.1)}` |
| 端口 | `${__P(port,12300)}` |
| 复用连接 | 勾选 |
| 请求后关闭连接 | 不勾选 |
| EOL 字节 | 10，即 LF |
| NoDelay | 勾选 |
| 连接 / 响应超时 | 3000 / 5000 ms |
| 发送内容 | ping 后按一次 Enter，发送真实 LF |

本轮约定单行消息，每个连接完整收到回声后再发下一条。TCPClientImpl 根据 EOL 结束读取；该配置只约束压测端，服务器仍按字节回显。添加响应正文断言，使用 Matches 匹配正则 `ping\n`，确认完整回声；发送框中的 LF 是真实换行，断言中的 `\n` 是正则表示。[TCP 与断言组件说明](https://jmeter.apache.org/usermanual/component_reference.html)

## C：连接放行与联调

确认本机实际发起连接时的公网出口 IPv4。云安全组入站规则为 TCP 12300，来源限定该地址的 /32；出口地址变化后更新对应规则。主机防火墙采用相同来源限制。

若服务器已经使用且启用 UFW，将下例中的地址替换后执行；其他防火墙使用其现有管理方式，不切换防火墙体系。

```sh
bench_client_cidr='本机公网IPv4/32'
sudo ufw status verbose
sudo ufw allow proto tcp from "$bench_client_cidr" to any port 12300
sudo ufw status numbered
```

以上不是启用 UFW 的命令；保留现有 SSH 访问规则与防火墙启用状态。[UFW 规则语法](https://manpages.ubuntu.com/manpages/noble/man8/ufw.8.html)

重新启动并确认服务器 READY 后，在本机运行：

```sh
nc 云服务器公网IPv4 12300
```

输入多行，确认同一连接持续回显；再用 JMeter 单线程验证。连不上时依次核对进程与监听地址、云安全组、主机防火墙、本机公网出口地址和本机出站规则。

## D：压测与观察

保存 echo.jmx 后，在本机执行。以下是 Linux/macOS Shell 写法；Windows 使用 JMeter 对应启动脚本并调整命令换行。每轮更换文件名，报告目录必须不存在或为空。

```sh
mkdir -p build/bench-results
jmeter -n -t echo.jmx \
    -Jhost=云服务器公网IPv4 -Jport=12300 \
    -Jusers=50 -Jramp=10 -Jduration=120 \
    -Jtcp.charset=UTF-8 \
    -l build/bench-results/w1-c50-ping-r1.jtl \
    -j build/bench-results/w1-c50-ping-r1.log \
    -e -o build/bench-results/w1-c50-ping-r1-report
```

建议先独立运行一轮 30 秒预热，再正式采集；初轮按完整 120 秒报告，包含 10 秒爬升。若后续统计稳态，应按 JTL 时间戳统一截取区间并标注口径。

| 维度 | 起始范围 |
|---|---|
| 服务端工作线程 | 0、1、2，分别重启服务 |
| 客户端并发 | 1、10、50、100，逐步增加 |
| 消息 | ping+LF 验证后，固定 64 B、1024 B，长度包含 LF |
| 内容 | 单行 ASCII，仅末尾一个 LF；更换内容时同步调整断言 |
| 重复 | 每组至少 3 次，保留各次结果及汇总 |

一次发送后完整收到并校验回声，计为一个成功事务；成功事务数除以测量秒数得到本轮成功 TPS/QPS。失败率单独报告，吞吐统一使用每秒单位。长连接与短连接测试分开记录。

服务器另开终端观察，在仓库根目录读取本次 PID：

```sh
bench_server_pid=$(cat build/bench/run/echo.pid)
pidstat -u -r -p "$bench_server_pid" 1
```

每核 CPU 和网卡流量分别在其他终端观察：

```sh
mpstat -P ALL 1
```

```sh
sar -n DEV 1
```

比较前后 fd 数时，可以在独立终端执行；确认客户端任务已结束后再检查资源回收。

```sh
bench_server_pid=$(cat build/bench/run/echo.pid)
find "/proc/$bench_server_pid/fd" -mindepth 1 -maxdepth 1 | wc -l
```

每组记录 commit/本地补丁、日志级别、线程数、并发、实际消息字节数、时长、成功事务/秒、P95/P99、错误率、CPU/RSS、fd 数及网络吞吐，同时观察本机 JMeter CPU/内存。公网 RTT 和云出网带宽可能先于服务端 CPU 成为瓶颈，这份数据表示当前两端与路径的表现。

测试完成后停止自己的实例、保留日志与结果，再移除为本轮新增的云安全组来源规则。若使用上面的 UFW 规则，核对来源地址后删除同一条规则：

```sh
bench_client_cidr='本机公网IPv4/32'
sudo ufw delete allow proto tcp from "$bench_client_cidr" to any port 12300
```

结果和环境信息收齐后，再讨论是否需要同地域压测、专用客户端或针对性优化。
