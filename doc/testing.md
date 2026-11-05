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

当前主构建登记 29 项测试，含已有 `_ndebug` 专项目标。脚本固定使用 Debug，不将协程原型视为已通过 Release 回归；模块状态与暂缓事项见 [项目进度](progress.md)。

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
