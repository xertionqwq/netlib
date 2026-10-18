# AGENTS.md

本文件是本仓库所有 AI 助手共用的协作约定。模块状态、当前路线和暂缓事项见 [doc/progress.md](doc/progress.md)，学习笔记见 [doc/learning-log/](doc/learning-log/)。涉及结构、进度或实现方向时，先阅读进度文档，再核对源码。

## 项目定位

- C++17、Linux、header-only 网络库，通过对照 muduo 学习 Reactor，后续再探索协程整合。
- 无第三方库依赖，使用 C++ 标准库与 Linux/POSIX 接口，包括 epoll、eventfd、socket 和 ucontext。
- 构建入口为 CMake；公共头文件位于 `include/netlib/`，测试位于 `tests/`，示例位于 `examples/`。
- 文档和构建步骤使用仓库相对路径，不依赖个人机器上的参考仓库或仓库外笔记。

## 协作方式

- 用户主导学习、核心实现和架构决策；助手负责讲解、源码对比、审查、小范围修复与测试。
- 用户明确要求实现某个模块时，在已授权范围内完成代码和验证；不要因为新增文件或修改接口而重复索要确认。
- 未经要求，不全量代写后续模块，不扩大为架构重构。出现超出授权范围的重大设计变化时，说明影响并确认方向。
- 方向尚未确定时，比较至少两种有实际意义的方案并给出推荐理由；用户已经选定方案后直接推进。
- 保留用户已有修改，只调整与当前任务相关的内容。进度文档中标为暂缓的问题，不主动扩展处理。
- 执行前评估副作用；沟通重要决策、改动效果和验证结果，无需输出内部推理过程。
- 提交和推送按用户明确要求执行，分别说明本地提交与远端推送的结果。

## 代码约定

- 类名使用 PascalCase，成员变量以下划线结尾；匹配所在文件的缩进、命名和注释风格。
- 业务类型放在 `netlib` 命名空间；`CurrentThread` 等已有辅助命名空间可以保留。
- 新头文件使用 `NETLIB_<CLASSNAME>_H` 风格的头文件保护。保持 header-only，新增类外函数定义使用 `inline`，共享变量按需使用 C++17 inline 变量，避免多翻译单元重复定义。
- 独占资源通常使用 `unique_ptr`；连接的共享生命周期使用 `shared_ptr`。借用指针必须明确有效期，不能据此释放对象。
- 需要禁止拷贝的资源类私有继承 `Noncopyable`，不重复声明删除的拷贝操作；移动能力单独设计。
- 回调使用 `std::function` 类型别名，按值接收后按需 `std::move`；必需回调与可选回调分别处理。
- Channel/Poller 的注册更新和移除在所属 EventLoop 线程执行；跨线程任务通过 `runInLoop()` 或 `queueInLoop()` 提交，同时保证对象存活。
- 不把必须执行的系统调用放入标准 `assert`。需要在 NDEBUG 下生效的内部检查使用 `NETLIB_ASSERT`；可恢复错误根据接口约定处理。
- 保持修复范围最小，不因发现旧原型未完全符合约定而顺带大范围重构。

## 日志约定

- 保持同步流式接口，Logger 析构时统一输出到 stderr；时间获取和字符串格式化交给 Timestamp。
- 普通日志在构造 Logger、求值日志表达式之前检查级别；已接受的消息不在析构时重新过滤。
- Logger 保存和恢复日志表达式开始时的 errno；系统调用失败后，需要立即保存错误码供后续使用。
- FATAL 始终输出，刷新后调用 `abort()`；`NETLIB_ASSERT` 条件只求值一次，并在 NDEBUG 下生效。

## 构建与验证

从仓库根目录执行：

```sh
cmake -S . -B build -DNETLIB_BUILD_TESTS=ON -DNETLIB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure --timeout 30
```

常用定向验证：

```sh
ctest --test-dir build -R '^(test_channel|test_epollpoller|test_eventloop|test_eventloopthread|test_eventloopthreadpool)(_ndebug)?$' --output-on-failure
ctest --test-dir build -R '^test_thread$' --output-on-failure
ctest --test-dir build -R '^test_log(_ndebug)?$' --output-on-failure
ctest --test-dir build -R '^test_net$' --output-on-failure
ctest --test-dir build -R '^test_socket(_ndebug)?$' --output-on-failure
```

- 按变更运行相关测试，必要时验证示例构建和真实网络行为。涉及共享基础设施时运行完整回归。
- 并发生命周期、错误路径和 header-only 链接应有针对性验证；适用时覆盖多翻译单元及 NDEBUG 构建。
- 新增 NDEBUG 测试的关键检查不要依赖标准 `assert`；不要编写仅复述实现的测试。
- 文档变更核对相对链接、源码事实及命令入口，无需重复编译未修改的代码。
- 测试数量和名称以 `CMakeLists.txt` 及 `ctest --test-dir build -N` 为准。

## 文档维护

- `AGENTS.md`：稳定的通用约束、协作规则、代码规范和验证命令。
- `CLAUDE.md`：要求读取通用规则的简短入口，避免复制整份约定。
- `doc/progress.md`：模块进度、已确定的实现决策、后续路线和暂缓问题；模块达到里程碑或方向变化时更新。
- `doc/learning-log/`：学习过程、原理讲解和源码阅读笔记。
- 进度区分“已实现”“最小实现”“已有原型”和“待完善”，不能仅根据存在文件就认定模块完整。
- 修改后说明具体文件、行为变化及验证结果；无法验证的部分如实说明。
