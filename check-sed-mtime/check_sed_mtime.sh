#!/bin/bash
# check_sed_mtime.sh - 检查 sed -i 修改文件后 mtime 是否总是更新
# 使用方法: ./check_sed_mtime.sh [迭代次数]

set -euo pipefail

# 默认迭代次数
ITERATIONS=${1:-1000}
TEST_DIR=$(mktemp -d)
TEST_FILE="$TEST_DIR/test.txt"
SED_CMD="sed"

# 清理临时目录
trap 'rm -rf "$TEST_DIR"' EXIT

cd "$TEST_DIR"
echo "Initial content" > "$TEST_FILE"

# 显示系统环境信息
echo "=== 系统信息 ==="
uname -a
$SED_CMD --version | head -n1
df -T . | tail -n1
echo "测试目录: $TEST_DIR"
echo "测试文件: $TEST_FILE"
echo "迭代次数: $ITERATIONS"
echo

# 函数：获取文件的 mtime 秒数和纳秒部分
get_mtime_ns() {
	local file=$1
	local sec=$(stat -c '%Y' "$file" 2>/dev/null)
	# 从可读格式中提取纳秒（例如 2025-03-25 10:20:30.123456789 +0800）
	local nsec=$(stat -c '%y' "$file" 2>/dev/null | awk '{print $2}' | cut -d. -f2 | tr -d ' ')
	if [[ -z $nsec ]]; then
		nsec=0
	fi
	echo "$sec $nsec"
}

# 初始化前一次的值
prev_mtime=$(get_mtime_ns "$TEST_FILE")
prev_inode=$(stat -c '%i' "$TEST_FILE")

declare -i same_mtime=0
declare -i total=0

echo "开始测试，请稍候..."

for ((i=1; i<=ITERATIONS; i++)); do
	# 使用 sed 修改文件内容（每次写入不同的数字，确保内容变化）
	if ! $SED_CMD -i "s/.*/$i/" "$TEST_FILE" 2>/dev/null; then
		echo "警告: sed 命令在第 $i 次迭代失败，跳过本次" >&2
		continue
	fi

	curr_mtime=$(get_mtime_ns "$TEST_FILE")
	curr_inode=$(stat -c '%i' "$TEST_FILE")

	if [[ "$curr_mtime" == "$prev_mtime" ]]; then
		same_mtime=$((same_mtime + 1))
		echo "===== 第 $i 次迭代：mtime 未变化 ====="
		echo "  前一次 mtime: $prev_mtime (纳秒)  inode: $prev_inode"
		echo "  当前  mtime: $curr_mtime (纳秒)  inode: $curr_inode"
		echo "  文件内容: $(cat "$TEST_FILE")"
		echo
	fi

	prev_mtime=$curr_mtime
	prev_inode=$curr_inode
	total=$((total + 1))
done

echo "=== 测试结果 ==="
echo "有效迭代次数: $total"
echo "mtime 未变化次数: $same_mtime"
if [[ $total -gt 0 ]]; then
	echo "未变化概率: $((same_mtime * 100 / total))%"
fi
