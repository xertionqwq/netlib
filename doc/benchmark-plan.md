# 回声压测计划与结果

编写日期：2026-11-06；更新日期：2026-11-07。两轮跨端与回环压测已完成，结果与结论见文末两节。前文保留可复用的准备与采集流程，供后续轮次参考；实际环境、配置及统计口径以各轮记录为准。

目标环境为 2 核、2 GB 云服务器运行 echo_server，本机运行 JMeter，经公网连接 TCP 12300。先建立可重复的性能基线，记录完整回声事务吞吐、延迟、错误率及资源占用。功能回归见 [testing.md](testing.md)，连接关闭边界见 [design.md](design.md)。

## 执行顺序与分工

| 阶段 | 执行位置 | 完成条件 |
|---|---|---|
| A：服务器准备 | 云服务器 | 同步代码、检查依赖、通过回归、构建 Release、完成回环收发与停止验证 |
| B：客户端准备 | 本机 | 安装 JDK/JMeter、创建测试计划、确认实际公网出口 IPv4 |
| C：连接放行 | 云控制台与服务器、本机 | 仅对本机出口地址放行 TCP 12300，本机确认可持续回声 |
| D：基线测试 | 两端配合 | 逐组压测、同步监控、保存配置及结果 |

上述阶段均已完成。跨端测试中，JMeter 或 WSL loopclient 运行在本机，云服务器运行被测程序与监控工具；回环测试中，临时 loopclient 与服务端共享云服务器的 2 vCPU。后续复跑时，先确认服务器环境和本机出口地址，再核对放行规则。

## A：服务器准备

### 1. 同步与检查环境

Shell 示例使用 Bash。仓库相关命令从仓库根目录执行；同步前先保留服务器已有修改，各轮实际使用的源码差异也应随版本记录保存。

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

当前 [examples/echo_server.cpp](../examples/echo_server.cpp) 已在 main 开头、创建任何 EventLoop 或工作线程之前设置 WARN（提交 `beb07cc`），复跑时核对即可：

```cpp
netlib::Logger::setLevel(netlib::LogLevel::WARN);
```

该设置只影响示例，库默认级别仍为 DEBUG，尚无日志级别命令行参数。Channel 和 epoll 路径有频繁的 INFO 日志，仅重定向到 /dev/null 仍会发生格式化和加锁。WARN 不影响 stdout 的 READY/STOPPED 通知。两轮历史测试使用的是 `4fb1b1f` 加 WARN 本地补丁，需保留该版本信息。

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

这里只构建 Release 示例，功能验证使用上一步的 Debug 回归。所有临时构建、日志和本地补丁记录放在已忽略的 build/ 下。结果同时记录 commit 和源码差异；当前版本已包含 WARN 设置，无其他修改时 perf-logging.patch 为空，历史补丁另行保留。

### 4. 启动与回环验证

先检查端口，同一端口只启动一个测试实例；若已有进程监听，先核对其用途。

```sh
ss -lntp 'sport = :12300'
```

复跑先配置一个工作线程；之后分别对比 0、1、2 个工作线程。每次切换先停止本次实例，并另存上一组日志。

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

后续复跑或更换服务器环境时，记录以下信息，供客户端准备和连接验证使用：

- 发行版、CPU/内存、编译器/CMake 版本、源码 commit、实际日志级别与本地差异。
- Debug 回归结果、Release 构建结果、实际启动参数。
- READY、回环收发结果，以及 SIGTERM 后的 STOPPED 和退出情况。
- 云服务器公网 IPv4、公网出网带宽、现有主机防火墙类型与端口规则状态。

实际 IP 与运行结果在交接时提供，不将凭据或机器专属配置提交到仓库。

## B：本机 JMeter 准备

本轮已在 Windows 本机使用 JMeter 5.6.3 与 Temurin JDK 17，并完成跨端测试。复跑时先在 GUI 中以单线程调试；正式负载使用命令行，并关闭查看结果树等详细监听器。更换 JMeter 发行版时重新核对 Java 要求。[安装与 CLI 说明](https://jmeter.apache.org/usermanual/get-started.html#non_gui)

测试计划 echo.jmx 和启动脚本 run.ps1 已保存在客户端仓库外，尚未纳入本项目。以下保留复跑配置示例，使用现有计划前核对实际字段：线程组设置线程数 `${__P(users,10)}`、Ramp-up `${__P(ramp,10)}` 秒、持续时间 `${__P(duration,120)}` 秒，循环选择永远并启用持续时间。这些参数通过 -J 传入。[属性函数说明](https://jmeter.apache.org/usermanual/functions.html#__P)

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

复跑约定单行消息，每个连接完整收到回声后再发下一条。TCPClientImpl 根据 EOL 结束读取；该配置只约束压测端，服务器仍按字节回显。响应正文断言使用 Matches 匹配正则 `ping\n`，确认完整回声；发送框中的 LF 是真实换行，断言中的 `\n` 是正则表示。首轮实际记录为“包含 ping”断言，不能据此认定已做逐字节校验。[TCP 与断言组件说明](https://jmeter.apache.org/usermanual/component_reference.html)

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

复跑建议先独立运行一轮 30 秒预热，再正式采集。上述示例按完整 120 秒报告，包含 10 秒爬升；两轮历史结果记录为稳态 QPS。若统计稳态，应按 JTL 时间戳统一截取区间并标注起止时间、实际持续时间和成功/失败计数，避免混用全程 samples 与稳态 QPS。

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

本轮压测已结束，是否继续使用同地域客户端或开展针对性优化，后续按需要讨论。

## 本轮结果（2026-11-06）

### 环境与口径

- 代码：`4fb1b1f`，另加 `examples/echo_server.cpp` 的 WARN 日志补丁（记录在 `build/bench/run/perf-logging.patch`）；Release 构建，`-O3 -DNDEBUG`。
- 服务端：阿里云 ECS（2 vCPU / 2 GiB，Ubuntu 22.04），EIP 固定带宽 3 Mbps，运行 `--port 12300 --threads 1`，启动输出 `READY 0.0.0.0:12300`。
- 跨端客户端：Windows 本机 JMeter 5.6.3（Temurin JDK 17），TCPClientImpl，复用长连接，EOL=LF，NoDelay，连接/响应超时 3000/5000 ms；每次发送 5 字节 `ping\n`，响应断言包含 `ping`。
- 回环客户端：临时 C++17 epoll 非阻塞客户端，每连接 ping-pong 同样 5 字节；源码未纳入仓库，服务端保留在 `build/bench/loopclient.cpp`，客户端侧也留有一份副本。
- QPS 均为稳态值：10/200/500/1000 并发采集 60 s，50/100 并发采集 120 s，每组先预热。本轮未按原计划做 3 次重复。

两轮数值均整理自已有测试记录。原始 JTL、监控文件、测试计划及临时客户端源码未纳入仓库；本次文档同步未重新分析原始数据或复跑。首轮断言为包含匹配，loopclient 的完整回声校验方式尚未在本文记录；表中 QPS 和 errors 保留工具原有口径。后续复跑再统一为完整校验后的成功事务/秒。

### 跨端（公网，EIP 出向 3 Mbps）

| 并发 | QPS | avg | p95 | p99 | max | 备注 |
|---|---|---|---|---|---|---|
| 10 | 269 | 34 ms | 40 ms | 42 ms | — | |
| 50 | 1,424.6 | — | 39 ms | 42 ms | — | |
| 100 | 2,742 | — | 42 ms | 72 ms | — | |
| 200 | 5,607 | 35.6 ms | 40 ms | 43 ms | — | 出向约 2.65 Mbps |
| 500 | 6,255 | 73.7 ms | 283 ms | 806 ms | 1,848 ms | |
| 1000 | 6,007 | 145 ms | 792 ms | 1,470 ms | 5,015 ms | 29 个 5 s 超时（0.01%） |

除上述超时外，JMeter 侧业务错误为 0。1000 并发期间服务端：系统 idle 88–93%，`%sys` 2–4%，`%soft` 3–5%；echo_server 约 8% CPU、RSS 5–7 MB；eth0 出向约 10–11k pps。

带宽实测：EIP 出方向硬上限约 3 Mbps（4 条并行 8 MB 下载聚合 3.2 Mbps，单流 3.3 Mbps）；入方向约 64 Mbps，不受该限制。

### 本机回环（客户端与服务端共享 2 vCPU）

| 客户端进程 | 连接数 | QPS | echo_server CPU |
|---|---|---|---|
| 1 | 128 | 98,469 | 85% |
| 1 | 512 | 75,057 | 88% |
| 1 | 1024 | 69,694 | 91% |
| 1 | 2048 | 66,387 | 90% |
| 2 | 2×64（共 128） | 105,495 | 92% |
| 2 | 2×256（共 512） | 77,682 | 93% |
| 2 | 2×512（共 1024） | 78,067 | 95% |

全部 0 错误。CPU 饱和时 user+sys 合计 92–97%，system 占比高于 user。

### 结论

1. 本轮吞吐平台与 EIP 出向带宽限制一致：`QPS ≈ 3,000,000 bit/s ÷ (约 60 B/事务 × 8 bit/B) ≈ 6.2k/s`，接近 500/1000 并发实测值。每事务约 60 B 仅为包含消息及协议开销的粗略估算，未逐包核对，也未验证云侧带宽计量口径。
2. 低并发时 QPS 由 RTT 和并发决定：每连接约 27–29 QPS，对应 RTT 约 35 ms；约 200 并发时触及带宽上限（约 2.65 Mbps），之后 QPS 进入平台，延迟随排队上升。
3. 1000 并发时服务端 CPU 约 8%，且回环可达到更高吞吐，当前记录未测得服务端处理上限。延迟升高和少量超时与公网带宽受限、排队相符，具体发生位置尚未验证。
4. 回环配置单 IO 线程，观测到的最高吞吐为 105,495 QPS（2 客户端 × 64 连接），此时服务端 CPU 约 92%；连接数继续增加后回落到 6.6–7.8 万 QPS。
5. 回环客户端与服务端共享 2 vCPU，CPU 占用较高，且本轮未测试 `--threads 2`。10.5 万是该测试组合的最高观测值，尚未隔离客户端、调度及内核开销，不能当作独立服务端峰值。
6. 除表中 29 个超时外，JMeter 未记录其他业务错误。本轮收尾时服务端关闭路径记录了约 1.1 万条 ENOTCONN/ECONNRESET/EPIPE（集中在 75 s 内），发生于客户端批量断连期间；日志未与事务逐一关联，不计入 JMeter 错误数，也不能据此单独确认全部由 RST 引起。

### 后续建议

- 公网若继续加码：EIP 改为按使用流量计费并提升峰值到 50 Mbps，用 C++ 客户端替代 JMeter（3000+ 线程过重），按 `QPS ≈ 连接数 ÷ RTT` 估算，35 ms RTT 下 1500–3500 连接约对应 4.3 万–10 万 QPS；4–8 GB 出向流量约 ¥3–7（按 0.8 元/GB 估算）。（11-07 已按此升级，结果见下一节。）
- 回环若测服务端上限：绑核、使用多进程轻量客户端、补测 `--threads 2`，并给客户端增加延迟分位数统计（当前 loopclient 只统计 QPS 和错误）。
- 若需长期复用 loopclient，应纳入仓库并增加构建入口；本轮它只是 `build/` 下的一次性工具。
- 本轮未做多次重复与跨地域对比，结论只代表当前客户端、路径和 3 Mbps 带宽配置。

### 产物与现状（2026-11-06 晚）

- 客户端（仓库外）：JMeter 测试计划 `echo.jmx`、启动脚本 `run.ps1`、`loopclient.cpp`、说明 `README.md`、汇总 `results/summary.csv` 及各轮 `.jtl`/`.log`。
- 服务端（`build/`，已被 Git 忽略）：`build/bench/echo_server`（Release，约 202 KB）、`build/bench/loopclient`（约 17.6 KB）、`build/bench/run/` 下的 `echo.stdout`/`echo.stderr`、`mon2.sh`、`startmon2.sh` 与监控输出文件。
- 当前状态：echo_server 已停止，12300 无监听；安全组 12300 规则保留。客户端为动态家宽出口，次日测试前需重新核对出口 IP 并更新对应 /32 规则。
- 当时安排：次日核对出口 IP、启动服务并继续测试；已于 11-07 升到 50 Mbps 执行，见下一节。

## 第二轮结果（2026-11-07）：50 Mbps 公网与手机热点对比

### 背景与变更

承接上一轮 3 Mbps 基线（5 B ping-pong 公网平台约 6.2k QPS）和本机回环约 10.5 万 QPS：本日将 EIP 从固定 3 Mbps 改为按使用流量、峰值 50 Mbps，继续跨端测试；同时用手机热点比较不同客户端路径。`scp` 50 MiB 用时 9.28 s（约 45.2 Mbps），确认升级生效。压测现已结束，服务端 echo_server 已停止。

### 环境与口径

- 服务端：阿里云 ECS（2 vCPU / 2 GiB，Ubuntu 22.04，cn-beijing），HEAD `4fb1b1f` 加 WARN 本地补丁，Release 构建，运行 `build/bench/echo_server --port 12300 --threads 1`。
- 客户端 A（家宽）：Windows 本机 JMeter 5.6.3 + JDK 17（TCP Sampler，5 B `ping` + LF，复用连接，EOL=10）；WSL Ubuntu + C++ loopclient（epoll 非阻塞，每连接 ping-pong，payload 可调）。
- 客户端 B（手机热点）：因 12300 安全组只放行家宽出口，临时使用已放行的 3389 端口跑 echo_server，测后停止；热点 ping 57 ms（45–98 ms），`scp` 上传 5 MB 用时 2.51 s（约 16.71 Mbps）。
- 监控：`ss` 连接数、`pidstat`（echo_server）、`/proc/net/dev`（`mon2.sh` / `mon3.sh`）。
- 表中 QPS 沿用原记录的稳态值；JMeter samples/errors 是否与稳态窗口完全一致、各组重复次数及逐字节校验方式尚未在本文记录，暂不视为严格可复现的性能基准。

### 公网（家宽，50 Mbps）

**JMeter（5 B 请求 / 5 B 响应）**

| 并发 | samples | errors | QPS（稳态） | p95 |
|---|---|---|---|---|
| 1000 | 1,453,216 | 0 | 27,510.5 | 41 ms |
| 2000 | 1,615,386 | 8（约 0.0005%） | 28,682.4 | 96 ms |

**WSL loopclient（5 B ping-pong，60 s）**

| 连接数 | QPS | errors | 连接数 / QPS 估算周期 |
|---|---|---|---|
| 1000 | 27,055.8 | 0 | ≈37 ms |
| 2000 | 29,298.8 | 43 | ≈68 ms |
| 3000 | 27,937.1 | 19 | ≈107 ms |
| 2×1500（两进程） | 12,279.4 + 15,580.4 = 27,859.8 | 75 / 0 | 不叠加 |

- 1000–3000 连接下吞吐形成约 27–29k QPS 的平台。表中周期按名义连接数除以 QPS 估算，假设每连接持续保持一个请求在途，包含客户端处理、网络往返、排队及服务端处理时间；它不是独立测量的网络 RTT，也不能证明存在单一队列或定位限制点。连接数峰值未达到名义值时，该估算也会有偏差。
- 3000 连接时服务端：echo_server CPU 30–34%（按单核 100% 口径），系统 idle 67–72.5%；eth0 收/发各约 29–33k pps、约 17 Mbps；实际连接数峰值 2981。当前 CPU 数据未显示服务端计算饱和，仍需排查整条路径和云侧网络限制。

### 本机回环（客户端与服务端共享 2 vCPU）

- 127.0.0.1，1000 连接 30 s：71,506.1 QPS；私网地址：70,359.9 QPS。
- 11-06 历史：128 连接 98,468.8 QPS；两客户端并行合计 105,495.1 QPS。
- `--threads` 对比（本机 1000 连接 30 s）：0 → 71,804 QPS；1 → 71,617；2 → 78,102。客户端与服务端同机抢 2 vCPU，提升有限。
- 回环观测吞吐约 70k–105k QPS，CPU 占用较高；客户端、服务端与内核共享两核，尚未隔离各自开销或测得独立服务端峰值。
- 访问自身 EIP：本机测试记录 TCP 握手 1000/1000 成功但 0 回声事务，原因未进一步验证。本次不将该路径用于容量结论，也不据此概括 EIP 是否支持 hairpin；后续可使用回环或另一台云主机作对照。

### 50 Mbps 带宽与 payload 扫描（家宽，2000 连接，60 s）

| payload | QPS | payload 吞吐 | 含协议开销的带宽估算 | 现象与推测 |
|---|---|---|---|---|
| 5 B | 29,298.8 | 1.2 Mbps | 约 14 Mbps | 约 29k 事务/秒 |
| 64 B | 29,368.8 | 15.04 Mbps | 约 27 Mbps | 约 29k 事务/秒 |
| 256 B | 19,332.5 | 39.59 Mbps | 约 48–49 Mbps | 接近 50 Mbps 带宽限制 |
| 1024 B | 5,252.9 | 43.03 Mbps | 约 45 Mbps | 接近 50 Mbps 带宽限制 |

- payload 吞吐按 `QPS × payload 字节数 × 8` 计算，为单向值；含协议开销的数值沿用原记录估算，未逐包核对，不能当作实测线速。
- 5/64 B 约 29k QPS，256/1024 B 随 payload 增大而下降，带宽估算接近 50 Mbps。转折位于已测的 64–256 B 区间，未补测区间内数据，不能精确定位到 200 B。
- 从 3 Mbps 升到 50 Mbps 后，5 B 吞吐从约 6.2k 提高到约 29k QPS，随后出现新的平台；256/1024 B 则呈现接近带宽上限的特征。现有记录无法据此预测继续提升带宽后的表现。

### 手机热点对比

- 5 B loopclient：100 连接 1,944.2 QPS（按连接数 / QPS 估算周期约 51 ms）；500 连接 4,166.5；1000 连接 4,502.7；2000 连接 4,659.8。
- payload（500 连接，30 s）：256 B → 4,246.7 QPS / 8.70 Mbps；1024 B → 2,828.2 QPS / 23.17 Mbps。
- 热点路径的 5 B 吞吐平台约 4.6k QPS。按每事务一个请求包、一个响应包粗算约 9k pps 往返，未计 ACK 或重传，不能直接当作链路包速率上限。scp 上传 16.7 Mbps 是另一次传输的结果，不能单独证明回声测试的限制原因。
- 保持同一服务端与 EIP，更换客户端网络并将端口从 12300 改到 3389 后，吞吐从约 29k 降到约 4.6k，说明测试路径和条件会影响结果。该对照尚不能将家宽平台定位到路由器或 ISP，也不能排除云侧包速率限制；手机热点适合补充路径对比，测服务端容量应另用更可控的客户端环境。

### 结论

1. 2 核 2 GiB 上，客户端与服务端同机时观测到约 70k–105k QPS、较高 CPU 占用；该数值属于当前测试组合，尚未测得独立服务端极限。
2. 带宽从 3 Mbps 升至 50 Mbps 后，5 B 公网吞吐从约 6.2k 提高到约 29k QPS；256/1024 B 数据呈现接近 50 Mbps 带宽限制的特征。
3. 公网小包平台低于已观察到的回环吞吐，CPU 数据未显示服务端计算饱和，具体限制位置仍待验证。家宽与热点对照不足以排除云侧包速率等限制，不能直接认定为客户端接入瓶颈。
4. 连接数 / QPS 只能估算闭环请求周期，不能作为独立 RTT 测量或单一队列的证据。继续定位时可增加另一台同地域云主机，对内网和公网分别测试，并记录客户端资源、服务端资源及网络指标。
5. 安全提醒：默认安全组中 3389（RDP）和 22（SSH）对 0.0.0.0/0 开放，建议收紧；12300 为临时压测规则，测试结束后可删除。

### 产物与现场

- 客户端（仓库外）：完整报告 `qps-report-20261107.md`；JMeter 结果 `w1000u-50m.jtl` 与 `.log`、`w2000u-50m.jtl`（约 150 MB）与 `.log`；loopclient 结果 `wsl-loop-1000/2000/3000c.txt`、`wsl-dual-*.txt`、`wsl-bw-64/256/1024.txt`、`summary.csv`；工具 `run.ps1`、`loopclient.cpp`、`loopclient2.cpp`。
- 服务端（`build/`，Git 忽略）：`build/bench/loopclient`、`build/bench/loopclient2`、`build/bench/run/mon2.sh`、`/tmp/mon3.sh`（未入库）。
- 现场：echo_server 已停止；安全组 12300 规则保留（来源为家宽 IP），压测结束后可删除。
