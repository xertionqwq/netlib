# 安装与外部项目接入

netlib 当前保持 header-only，安装内容是公共头文件与 CMake 包配置。使用方通过 `netlib::netlib` 获取头文件路径、C++17 要求和 `Threads::Threads` 依赖。

构建环境为 Linux、支持 C++17 的编译器及 CMake 3.14 或更新版本。安装包含当前 `include/netlib/` 下的公共头文件，各模块成熟度与生命周期约束见 [项目进度](progress.md)。

## 安装到指定目录

在仓库根目录执行：

```sh
cmake -S . -B build/install \
    -DNETLIB_BUILD_TESTS=OFF \
    -DNETLIB_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/build/prefix"
cmake --build build/install --target install
```

这会安装到仓库下的 `build/prefix/`，无需系统目录写入权限。`install` target 的写法兼容项目声明的 CMake 3.14 最低版本。

安装内容包括：

```text
<prefix>/
├── include/netlib/*.h
└── <libdir>/cmake/netlib/
    ├── netlibConfig.cmake
    └── netlibTargets.cmake
```

`<libdir>` 由 GNUInstallDirs 的 `CMAKE_INSTALL_LIBDIR` 决定。可在首次配置时传入 `-DCMAKE_INSTALL_INCLUDEDIR=include`、`-DCMAKE_INSTALL_LIBDIR=lib` 等参数覆盖目录布局。使用相对目录时，头文件与包配置都随安装前缀整体迁移；迁移后重新配置使用方即可。

需要安装到系统目录时，将 `CMAKE_INSTALL_PREFIX` 改为目标目录，例如 `/usr/local`，并使用具备该目录写入权限的账户执行安装命令。

## 在独立项目中使用

独立项目的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.14)
project(netlib_consumer LANGUAGES CXX)

find_package(netlib CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE netlib::netlib)
```

`main.cpp` 示例：

```cpp
#include <netlib/buffer.h>
#include <iostream>

int main() {
    netlib::Buffer buffer;
    buffer.append("hello netlib");
    std::cout << buffer.retrieveAllAsString() << '\n';
}
```

假设独立项目位于仓库同级的 `../my_app/`，从 netlib 仓库根目录执行：

```sh
cmake -S ../my_app -B ../my_app/build \
    -DCMAKE_PREFIX_PATH="$PWD/build/prefix"
cmake --build ../my_app/build -j
../my_app/build/my_app
```

`CMAKE_PREFIX_PATH` 指向安装前缀；也可用 `netlib_DIR` 直接指定包含 `netlibConfig.cmake` 的目录。包配置会自动查找线程依赖，使用方只需链接 `netlib::netlib`。

当前项目尚未定义发布版本，因此使用不带版本号的 `find_package(netlib CONFIG REQUIRED)`。

## 自动验证安装

在仓库根目录运行：

```sh
scripts/test_install.sh -j 2
```

脚本安装到临时目录，检查两种目录布局、安装前缀迁移，以及独立使用方的 Debug/Release 编译运行。脚本选项、日常回归与失败现场说明见 [测试脚本](testing.md)。

## 将源码加入父项目

将 netlib 源码放在父项目的 `third_party/netlib/` 后，可以直接使用同名 target：

```cmake
add_subdirectory(third_party/netlib)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE netlib::netlib)
```

测试和示例默认关闭，父项目可按需设置 `NETLIB_BUILD_TESTS` 和 `NETLIB_BUILD_EXAMPLES`。
