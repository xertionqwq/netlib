#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$repo_root/build"
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
timeout=30
regex=

usage() {
    cat <<'EOF'
用法：scripts/test.sh [选项]

配置并构建 Debug 测试与示例，然后运行 CTest（含已有 NDEBUG 专项目标）。
  -B, --build-dir DIR  构建目录，默认仓库下的 build；相对路径基于调用目录
  -j, --jobs N         构建并行数，默认 CMAKE_BUILD_PARALLEL_LEVEL 或 4
  -R, --regex REGEX    按测试名称筛选，未匹配任何测试时失败
      --timeout N     CTest 默认超时秒数，默认 30；用例自身的 TIMEOUT 优先
  -h, --help          显示帮助
EOF
}

usage_error() {
    printf '参数错误：%s\n' "$1" >&2
    exit 2
}

while (( $# )); do
    case "$1" in
        -B|--build-dir|-j|--jobs|-R|--regex|--timeout)
            (( $# >= 2 )) || usage_error "$1 缺少参数"
            [[ -n $2 ]] || usage_error "$1 的参数不能为空"
            case "$1" in
                -B|--build-dir) build_dir=$2 ;;
                -j|--jobs) jobs=$2 ;;
                -R|--regex) regex=$2 ;;
                --timeout) timeout=$2 ;;
            esac
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage_error "未知选项 $1" ;;
    esac
done

[[ $jobs =~ ^[1-9][0-9]*$ ]] || usage_error '并行数必须是正整数'
[[ $timeout =~ ^[1-9][0-9]*$ ]] || usage_error '超时必须是正整数'
for command in cmake ctest; do
    command -v "$command" >/dev/null || {
        printf '缺少命令：%s\n' "$command" >&2
        exit 1
    }
done

cmake -S "$repo_root" -B "$build_dir" \
    -DNETLIB_BUILD_TESTS=ON -DNETLIB_BUILD_EXAMPLES=ON \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --config Debug --parallel "$jobs"

test_args=(-C Debug --timeout "$timeout")
if [[ -n $regex ]]; then
    test_args+=(-R "$regex")
fi

# 在构建目录运行，兼容 CMake/CTest 3.14，无需较新的 --test-dir 选项。
(
    cd -- "$build_dir"
    test_list=$(ctest -N "${test_args[@]}")
    printf '%s\n' "$test_list"
    if [[ ! $test_list =~ Total\ Tests:\ ([0-9]+) ]] || (( BASH_REMATCH[1] == 0 )); then
        printf '没有选中可运行的测试。\n' >&2
        exit 1
    fi
    ctest --output-on-failure "${test_args[@]}"
)
