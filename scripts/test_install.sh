#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
timeout=10
keep=0

usage() {
    cat <<'EOF'
用法：scripts/test_install.sh [选项]

在临时目录安装 netlib，迁移安装前缀，并运行独立使用方的 Debug/Release 测试。
  -j, --jobs N     构建并行数，默认 CMAKE_BUILD_PARALLEL_LEVEL 或 4
      --timeout N 每项使用方测试的超时秒数，默认 10
      --keep      成功后也保留临时目录；失败时总会保留
  -h, --help     显示帮助

临时目录由 TMPDIR（默认 /tmp）决定；脚本不安装到系统目录。
EOF
}

usage_error() {
    printf '参数错误：%s\n' "$1" >&2
    exit 2
}

while (( $# )); do
    case "$1" in
        -j|--jobs|--timeout)
            (( $# >= 2 )) || usage_error "$1 缺少参数"
            case "$1" in
                -j|--jobs) jobs=$2 ;;
                --timeout) timeout=$2 ;;
            esac
            shift 2
            ;;
        --keep) keep=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage_error "未知选项 $1" ;;
    esac
done

[[ $jobs =~ ^[1-9][0-9]*$ ]] || usage_error '并行数必须是正整数'
[[ $timeout =~ ^[1-9][0-9]*$ ]] || usage_error '超时必须是正整数'
for command in cmake ctest mktemp cp mv rm cmp; do
    command -v "$command" >/dev/null || {
        printf '缺少命令：%s\n' "$command" >&2
        exit 1
    }
done

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/netlib-install-test.XXXXXX")
stage='准备独立使用方'
cleanup() {
    local status=$?
    if (( status != 0 )); then
        printf '安装验证失败：%s（退出码 %s）。\n' "$stage" "$status" >&2
    fi
    if (( keep || status != 0 )); then
        printf '保留验证目录：%s\n' "$work_dir"
    else
        rm -rf -- "$work_dir"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf '安装验证目录：%s\n' "$work_dir"
# 复制后再配置，避免使用方从仓库中意外获取头文件。
cp -R -- "$repo_root/tests/install_consumer" "$work_dir/consumer"

check_consumer() {
    local prefix=$1 libdir=$2 consumer_build=$3 config=$4
    local -a package_args
    if [[ $libdir == lib ]]; then
        package_args=("-DCMAKE_PREFIX_PATH=$prefix")
    else
        package_args=("-Dnetlib_DIR=$prefix/$libdir/cmake/netlib")
    fi
    stage="独立使用方 $config：$prefix"
    cmake -S "$work_dir/consumer" -B "$consumer_build" \
        "${package_args[@]}" \
        "-DNETLIB_EXPECTED_PREFIX=$prefix" \
        "-DNETLIB_INSTALL_TEST_TIMEOUT=$timeout" \
        "-DCMAKE_BUILD_TYPE=$config"
    cmake --build "$consumer_build" --config "$config" --parallel "$jobs"
    (
        cd -- "$consumer_build"
        test_list=$(ctest -N -C "$config")
        if [[ ! $test_list =~ Total\ Tests:\ ([0-9]+) ]] || (( BASH_REMATCH[1] == 0 )); then
            printf '独立使用方未注册测试。\n' >&2
            exit 1
        fi
        ctest -C "$config" --output-on-failure --timeout "$timeout"
    )
}

for layout in standard custom; do
    if [[ $layout == standard ]]; then
        includedir=include
        libdir=lib
    else
        includedir=headers
        libdir=lib64
    fi
    package_build="$work_dir/$layout-build"
    prefix="$work_dir/$layout prefix"
    relocated_prefix="$work_dir/$layout relocated prefix"
    stage="安装 $layout 布局"
    cmake -S "$repo_root" -B "$package_build" \
        -DNETLIB_BUILD_TESTS=OFF -DNETLIB_BUILD_EXAMPLES=OFF \
        -DCMAKE_BUILD_TYPE=Debug \
        "-DCMAKE_INSTALL_PREFIX=$prefix" \
        "-DCMAKE_INSTALL_INCLUDEDIR=$includedir" \
        "-DCMAKE_INSTALL_LIBDIR=$libdir"
    DESTDIR= cmake --build "$package_build" --target install --config Debug --parallel "$jobs"

    stage="核对 $layout 安装产物"
    for header in "$repo_root"/include/netlib/*.h; do
        cmp -- "$header" "$prefix/$includedir/netlib/${header##*/}"
    done
    for config_name in netlibConfig.cmake netlibTargets.cmake; do
        config_file="$prefix/$libdir/cmake/netlib/$config_name"
        [[ -f $config_file ]] || { printf '缺少安装配置：%s\n' "$config_file" >&2; exit 1; }
        config_text=$(<"$config_file")
        if [[ $config_text == *"$repo_root"* || $config_text == *"$package_build"* ||
              $config_text == *"$prefix"* ]]; then
            printf '导出配置包含构建前的绝对路径：%s\n' "$config_file" >&2
            exit 1
        fi
    done

    stage="迁移 $layout 安装前缀"
    mv -- "$prefix" "$relocated_prefix"
    for config in Debug Release; do
        check_consumer "$relocated_prefix" "$libdir" "$work_dir/$layout-$config" "$config"
    done
done

printf '安装验证通过：两种目录布局，迁移后的 Debug/Release 使用方均运行成功。\n'
