#!/usr/bin/env bash
# AutoRunner（代码工作区与脚本工作区分离版）
# 工作流：
# 1) 总是先“强制重新克隆”代码工作区（保证最新版本）
# 2) 在“代码工作区”的 workplan 中检查是否有“上次执行时间”以来新增的排期文件（文件名形如 YYYYMMDD_HHMM.txt）
#    - 若不存在“上次执行时间”，默认设置成“当前时间一小时前”
# 3) 对每个新增排期：在代码工作区执行编译+运行（调用代码工作区的 workplan/run.sh）
# 4) 日志与结果文件首先写入“脚本工作区”的 result 目录，然后复制一份到“代码工作区”的 result 目录
# 5) 在“代码工作区”执行 git add/commit/push（仅提交 result 下的结果）
#
# 可配置环境变量：
# - REPO_PATH              脚本工作区根目录（默认当前目录）
# - SCRIPT_WORKPLAN_DIR    脚本工作区的排期目录（默认 $REPO_PATH/workplan，仅用于存放本地排期，不在此处扫描）
# - SCRIPT_RESULT_DIR      脚本工作区的结果目录（默认 $REPO_PATH/result）
# - STATE_FILE             状态文件（默认 $REPO_PATH/scripts/state.json），仅用于记录 lastExecutionTime
# - CODE_WORKSPACE         代码工作区根目录（默认 $HOME/runner）
# - CODE_REPO_URL          克隆用的仓库地址（默认读取脚本工作区 origin；失败回退到 git@github.com:hccyril/AppDemoOfOpenFHE.git）
# - CODE_BRANCH            克隆分支（默认 main）
# - GIT_USER_NAME          代码工作区提交用户名（避免 GH007）
# - GIT_USER_EMAIL         代码工作区提交邮箱（建议使用 GitHub noreply 邮箱）
# - FETCH_TIMEOUT_SEC      网络操作的超时时间（默认 60）

set -euo pipefail

# -------- 基本配置 --------
REPO_PATH="${REPO_PATH:-$PWD}"

SCRIPT_WORKPLAN_DIR="${SCRIPT_WORKPLAN_DIR:-$REPO_PATH/workplan}"
SCRIPT_RESULT_DIR="${SCRIPT_RESULT_DIR:-$REPO_PATH/result}"
STATE_FILE="${STATE_FILE:-$REPO_PATH/scripts/state.json}"

CODE_WORKSPACE="${CODE_WORKSPACE:-$HOME/runner}"
CODE_REPO_DIR="${CODE_WORKSPACE}/AppDemoOfOpenFHE"

DEFAULT_REPO_URL="git@github.com:hccyril/AppDemoOfOpenFHE.git"
CODE_REPO_URL="${CODE_REPO_URL:-$(git -C "$REPO_PATH" config --get remote.origin.url 2>/dev/null || echo "$DEFAULT_REPO_URL")}"
CODE_BRANCH="${CODE_BRANCH:-main}"

FETCH_TIMEOUT_SEC="${FETCH_TIMEOUT_SEC:-60}"

log_ts() { date +"%Y-%m-%d %H:%M:%S"; }

# -------- 依赖检测（jq 可选）--------
if command -v jq >/dev/null 2>&1; then
  USE_JQ=1
else
  USE_JQ=0
fi

# -------- 目录准备 --------
mkdir -p "$SCRIPT_WORKPLAN_DIR" "$SCRIPT_RESULT_DIR" "$(dirname "$STATE_FILE")" "$CODE_WORKSPACE"

# -------- 读取 lastExecutionTime（秒）--------
# 若不存在，默认设置为“当前时间 - 3600 秒”
LAST_EXEC_TS=""
if [[ -f "$STATE_FILE" ]]; then
  if [[ "$USE_JQ" -eq 1 ]]; then
    LAST_EXEC_TS="$(jq -r '.lastExecutionTime // ""' "$STATE_FILE" 2>/dev/null || echo "")"
  else
    LAST_EXEC_TS="$(grep -oE '"lastExecutionTime"\s*:\s*[0-9]+' "$STATE_FILE" | awk -F':' '{print $2}' | tr -d ' ' || echo "")"
  fi
fi
if [[ -z "$LAST_EXEC_TS" || "$LAST_EXEC_TS" == "null" ]]; then
  LAST_EXEC_TS="$(date +%s)"
  LAST_EXEC_TS="$((LAST_EXEC_TS - 3600))"
fi

# -------- 将排期文件名转换为时间戳 --------
# 期望格式：YYYYMMDD_HHMM.txt
plan_name_to_epoch() {
  local name="$1"
  if [[ "$name" =~ ^([0-9]{8})_([0-9]{4})\.txt$ ]]; then
    local d="${BASH_REMATCH[1]}"
    local t="${BASH_REMATCH[2]}"
    local datestr="${d:0:4}-${d:4:2}-${d:6:2} ${t:0:2}:${t:2:2}:00"
    date -d "$datestr" +%s
  else
    echo 0
  fi
}

echo "[$(log_ts)] Preparing code workspace: $CODE_REPO_DIR (branch: $CODE_BRANCH)"

# -------- 强制重新克隆代码工作区（始终保证最新）--------
if [[ -d "$CODE_REPO_DIR" ]]; then
  echo "[$(log_ts)] Removing existing code repo directory..."
  rm -rf "$CODE_REPO_DIR"
fi

echo "[$(log_ts)] Cloning repository: $CODE_REPO_URL"
timeout "${FETCH_TIMEOUT_SEC}" git clone --branch "$CODE_BRANCH" "$CODE_REPO_URL" "$CODE_REPO_DIR" || {
  echo "[$(log_ts)] ERROR: git clone failed." >&2
  exit 1
}

# 初始化子模块（按 README）
if [[ -f "$CODE_REPO_DIR/.gitmodules" ]]; then
  echo "[$(log_ts)] Initializing submodules..."
  git -C "$CODE_REPO_DIR" submodule update --init --recursive || echo "[$(log_ts)] Submodule init failed, continuing..."
fi

# 配置提交身份与 filemode（避免权限位变化触发修改）
if [[ -n "${GIT_USER_NAME:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.name "$GIT_USER_NAME"
fi
if [[ -n "${GIT_USER_EMAIL:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.email "$GIT_USER_EMAIL"
fi
git -C "$CODE_REPO_DIR" config core.filemode false

# 当前克隆的提交（用于日志）
CLONED_HEAD="$(git -C "$CODE_REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"

# -------- 在“代码工作区”的 workplan 中扫描新增排期 --------
CODE_PLAN_DIR="$CODE_REPO_DIR/workplan"
if [[ ! -d "$CODE_PLAN_DIR" ]]; then
  echo "[$(log_ts)] No workplan directory found in code workspace: $CODE_PLAN_DIR"
  # 仍然更新 lastExecutionTime 为当前时间，避免下次重复扫描
  NOW_TS="$(date +%s)"
  if [[ "$USE_JQ" -eq 1 ]]; then
    jq -n --argjson ts "$NOW_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
  else
    printf '{"lastExecutionTime":%s}\n' "$NOW_TS" > "$STATE_FILE"
  fi
  exit 0
fi

mapfile -d '' CODE_PLANS_ALL < <(find "$CODE_PLAN_DIR" -maxdepth 1 -type f -name "*.txt" -print0 | sort -z)

NEW_PLANS=()
for pf in "${CODE_PLANS_ALL[@]}"; do
  [[ -z "${pf:-}" ]] && continue
  fname="$(basename "$pf")"
  ts="$(plan_name_to_epoch "$fname")"
  if [[ "$ts" -gt "$LAST_EXEC_TS" ]]; then
    NEW_PLANS+=("$pf")
  fi
done

if [[ "${#NEW_PLANS[@]}" -eq 0 ]]; then
  echo "[$(log_ts)] No new workplan to process since lastExecutionTime=$LAST_EXEC_TS"
  NOW_TS="$(date +%s)"
  if [[ "$USE_JQ" -eq 1 ]]; then
    jq -n --argjson ts "$NOW_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
  else
    printf '{"lastExecutionTime":%s}\n' "$NOW_TS" > "$STATE_FILE"
  fi
  exit 0
fi

echo "[$(log_ts)] Found ${#NEW_PLANS[@]} new workplan(s)."

# -------- 逐个执行排期 --------
MAX_TS_PROCESSED="$LAST_EXEC_TS"

for PLAN_FILE in "${NEW_PLANS[@]}"; do
  PLAN_NAME="$(basename "$PLAN_FILE")"
  PLAN_STAMP="${PLAN_NAME%.*}"
  PLAN_TS="$(plan_name_to_epoch "$PLAN_NAME")"
  if [[ "$PLAN_TS" -gt "$MAX_TS_PROCESSED" ]]; then
    MAX_TS_PROCESSED="$PLAN_TS"
  fi

  FULL_LOG_PATH="${SCRIPT_RESULT_DIR}/${PLAN_STAMP}_full.log"
  RUN_LOG_PATH="${SCRIPT_RESULT_DIR}/${PLAN_STAMP}_run.log"

  echo "[$(log_ts)] Processing plan: ${PLAN_NAME} (HEAD: ${CLONED_HEAD})"

  # 在代码工作区执行编译+运行；run.sh 位于代码工作区
  (
    cd "$CODE_REPO_DIR"
    {
      echo "=== AutoRunner started at $(log_ts) ==="
      echo "Script workspace: ${REPO_PATH}"
      echo "Code workspace: ${CODE_REPO_DIR}"
      echo "Branch: ${CODE_BRANCH}, HEAD: ${CLONED_HEAD}"
      echo "Workplan (code workspace): ${PLAN_FILE}"
      echo "----- Build & Run begin -----"

      # 捕获 run.sh 的“仅运行期输出”到 RUN_LOG_PATH，同时所有输出进入 FULL_LOG_PATH
      if [[ -x "$CODE_REPO_DIR/workplan/run.sh" ]]; then
        RUN_OUTPUT="$("$CODE_REPO_DIR/workplan/run.sh" "$PLAN_FILE" 2>&1 | tee /dev/fd/3 3>&1)"
      else
        # 若未赋可执行权限，则用 bash 直接执行
        RUN_OUTPUT="$(bash "$CODE_REPO_DIR/workplan/run.sh" "$PLAN_FILE" 2>&1 | tee /dev/fd/3 3>&1)"
      fi

      echo "----- Build & Run end -----"
      echo "=== AutoRunner finished at $(log_ts) ==="
    } 3> >(cat > "$RUN_LOG_PATH") | cat > "$FULL_LOG_PATH"
  ) || {
    echo "[$(log_ts)] ERROR during build/run for ${PLAN_NAME}; see full log: ${FULL_LOG_PATH}" >&2
  }

  # 将结果日志复制到代码工作区的 result 目录，以便提交推送
  mkdir -p "$CODE_REPO_DIR/result"
  cp -f "$FULL_LOG_PATH" "$CODE_REPO_DIR/result/" || true
  cp -f "$RUN_LOG_PATH" "$CODE_REPO_DIR/result/" || true

  # 在代码工作区提交并推送（仅 result）
  (
    cd "$CODE_REPO_DIR"
    git add result || true
    git commit -m "Auto result for ${PLAN_STAMP}" || echo "Nothing to commit."
    # 显式推送当前分支
    git push origin "$CODE_BRANCH" || echo "Push failed. Check Git auth."
  )
done

# -------- 更新 lastExecutionTime --------
NEW_LAST_EXEC_TS="$MAX_TS_PROCESSED"
if [[ "$USE_JQ" -eq 1 ]]; then
  jq -n --argjson ts "$NEW_LAST_EXEC_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
else
  printf '{"lastExecutionTime":%s}\n' "$NEW_LAST_EXEC_TS" > "$STATE_FILE"
fi

echo "[$(log_ts)] All done. Updated lastExecutionTime to ${NEW_LAST_EXEC_TS}"