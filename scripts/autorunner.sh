#!/usr/bin/env bash
# 主控脚本：
# - 定时执行：fetch/pull，如果有新指令文件则调用 run.sh
# - 生成两个日志文件：完整过程日志（full）与仅运行期输出（run）
# - 推送 result 和 state.json 到 GitHub
# - 以字典序方式处理一个未处理的最新计划文件（如需批量可自行改造）

set -euo pipefail

REPO_PATH="${REPO_PATH:-$PWD}"
WORKPLAN_DIR="${WORKPLAN_DIR:-workplan}"
RESULT_DIR="${RESULT_DIR:-result}"
RUN_SCRIPT="${RUN_SCRIPT:-workplan/run.sh}"
STATE_FILE="${STATE_FILE:-scripts/state.json}"
FETCH_TIMEOUT_SEC="${FETCH_TIMEOUT_SEC:-60}"

log_ts() { date +"%Y-%m-%d %H:%M:%S"; }

cd "$REPO_PATH"

# 目录准备
mkdir -p "$WORKPLAN_DIR" "$RESULT_DIR" "$(dirname "$STATE_FILE")"

# 读取或初始化状态
LAST_PROCESSED=""
if [[ -f "$STATE_FILE" ]]; then
  LAST_PROCESSED="$(jq -r '.lastProcessedPlan // ""' "$STATE_FILE" 2>/dev/null || echo "")"
fi

# Git fetch/pull
echo "[$(log_ts)] Fetching latest changes..."
# 设置超时（使用 timeout）
timeout "${FETCH_TIMEOUT_SEC}" git fetch --all --prune || echo "Fetch timeout or failed, continue..."
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
LOCAL_HEAD="$(git rev-parse HEAD)"
REMOTE_HEAD="$(git rev-parse "origin/${CURRENT_BRANCH}" || echo "")"

NEEDS_PULL="false"
if [[ -n "$REMOTE_HEAD" && "$LOCAL_HEAD" != "$REMOTE_HEAD" ]]; then
  NEEDS_PULL="true"
fi

if [[ "$NEEDS_PULL" == "true" ]]; then
  echo "[$(log_ts)] Pulling latest changes..."
  git pull --rebase
else
  echo "[$(log_ts)] No updates to pull."
fi

# 寻找新计划文件（字典序 > LAST_PROCESSED）
NEXT_PLAN=""
while IFS= read -r -d '' file; do
  name="$(basename "$file")"
  if [[ "$name" > "$LAST_PROCESSED" ]]; then
    NEXT_PLAN="$file"
    break
  fi
done < <(find "$WORKPLAN_DIR" -maxdepth 1 -type f -name "*.txt" -print0 | sort -z)

if [[ -z "$NEXT_PLAN" ]]; then
  echo "[$(log_ts)] No new workplan to process."
  exit 0
fi

PLAN_NAME="$(basename "$NEXT_PLAN")"
PLAN_STAMP="${PLAN_NAME%.*}"
FULL_LOG_PATH="${RESULT_DIR}/${PLAN_STAMP}_full.log"
RUN_LOG_PATH="${RESULT_DIR}/${PLAN_STAMP}_run.log"

echo "[$(log_ts)] Found new workplan: ${PLAN_NAME}"

# 执行并采集日志
{
  echo "=== AutoRunner started at $(log_ts) ==="
  echo "Repo: ${REPO_PATH}, Branch: ${CURRENT_BRANCH}"
  echo "Workplan: ${NEXT_PLAN}"
  echo "----- Build & Run begin -----"

  # 捕获 run.sh 的“仅运行期输出”
  RUN_OUTPUT="$("$RUN_SCRIPT" "$NEXT_PLAN" 2>&1 | tee /dev/fd/3 3>&1)"
  # 说明：
  # - tee 将输出复制到 fd 3（被此块捕获）和 stdout（进入完整日志）。
  # - RUN_OUTPUT 变量仅包含程序运行期输出（run.sh 最后执行的二进制输出）。

  echo "----- Build & Run end -----"
  echo "=== AutoRunner finished at $(log_ts) ==="
} 3> >(cat > "$RUN_LOG_PATH") | cat > "$FULL_LOG_PATH" || {
  echo "[$(log_ts)] ERROR during execution; see full log: ${FULL_LOG_PATH}" >&2
}

# 更新状态并推送
jq -n --arg lp "$PLAN_NAME" '{lastProcessedPlan: $lp}' > "$STATE_FILE"

git add "$RESULT_DIR" "$STATE_FILE"
git commit -m "Auto result for ${PLAN_STAMP}" || echo "Nothing to commit (maybe identical files)."
git push || echo "Push failed. Check Git auth."