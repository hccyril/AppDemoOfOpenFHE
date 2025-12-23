#!/usr/bin/env bash
# AutoRunner（彻底隔离版）
# 需求点实现：
# 1) 分离两个工作区：
#    - “脚本工作区”：当前仓库所在目录（REPO_PATH），用于扫描 workplan、收集日志（result）、记录状态（scripts/state.json）
#    - “代码工作区”：默认 $HOME/runner/AppDemoOfOpenFHE，每次执行前强制删除并重新 git clone，避免本地未提交改动导致 pull 失败
# 2) 每次执行：
#    - 在代码工作区重新克隆仓库（可配置分支）
#    - 从脚本工作区的 workplan 中筛选“上次执行时间”之后新增的排期文件（若无记录，默认“当前时间的1小时前”）
#    - 在代码工作区按 README 执行编译 + 运行（调用代码工作区里的 workplan/run.sh，参数为脚本工作区的排期文件路径）
#    - 日志在脚本工作区的 result 目录生成，同时复制一份到代码工作区的 result
#    - 在代码工作区执行 git add/commit/push，把 result 推送到 GitHub
#
# 可选配置（环境变量）：
# - REPO_PATH          脚本工作区根目录，默认当前目录
# - SCRIPT_WORKPLAN_DIR    脚本工作区的排期文件目录，默认 REPO_PATH/workplan
# - SCRIPT_RESULT_DIR      脚本工作区的结果目录，默认 REPO_PATH/result
# - STATE_FILE             脚本工作区的状态文件，默认 REPO_PATH/scripts/state.json
# - CODE_WORKSPACE         代码工作区的根目录，默认 $HOME/runner
# - CODE_REPO_URL          要克隆的仓库地址（默认从脚本工作区的 origin 获取；失败则使用 git@github.com:hccyril/AppDemoOfOpenFHE.git）
# - CODE_BRANCH            要克隆的分支，默认 main
# - GIT_USER_NAME          在代码工作区提交使用的用户名（避免 GH007，可配合 GIT_USER_EMAIL）
# - GIT_USER_EMAIL         在代码工作区提交使用的邮箱（建议使用 GitHub noreply 邮箱）

set -euo pipefail

# -------- 配置与路径 --------
REPO_PATH="${REPO_PATH:-$PWD}"

SCRIPT_WORKPLAN_DIR="${SCRIPT_WORKPLAN_DIR:-$REPO_PATH/workplan}"
SCRIPT_RESULT_DIR="${SCRIPT_RESULT_DIR:-$REPO_PATH/result}"
STATE_FILE="${STATE_FILE:-$REPO_PATH/scripts/state.json}"

CODE_WORKSPACE="${CODE_WORKSPACE:-$HOME/runner}"
CODE_REPO_DIR="${CODE_WORKSPACE}/AppDemoOfOpenFHE"

# 从脚本工作区仓库读取远程 URL（优先使用已有 origin）
DEFAULT_REPO_URL="git@github.com:hccyril/AppDemoOfOpenFHE.git"
CODE_REPO_URL="${CODE_REPO_URL:-$(git -C "$REPO_PATH" config --get remote.origin.url || echo "$DEFAULT_REPO_URL")}"

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

# -------- 状态读取：lastExecutionTime（秒）--------
# 若不存在，默认设为当前时间的 1 小时前
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
  LAST_EXEC_TS="$((LAST_EXEC_TS - 3600))"  # 默认回退一小时
fi

# -------- 工具函数：从计划文件名解析时间戳 --------
# 计划文件名格式：YYYYMMDD_HHMM.txt
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

# -------- 扫描“新增排期文件” --------
mapfile -d '' PLAN_FILES_ALL < <(find "$SCRIPT_WORKPLAN_DIR" -maxdepth 1 -type f -name "*.txt" -print0 | sort -z)

NEW_PLANS=()
for pf in "${PLAN_FILES_ALL[@]}"; do
  [[ -z "${pf:-}" ]] && continue
  local_name="$(basename "$pf")"
  ts="$(plan_name_to_epoch "$local_name")"
  if [[ "$ts" -gt "$LAST_EXEC_TS" ]]; then
    NEW_PLANS+=("$pf")
  fi
done

if [[ "${#NEW_PLANS[@]}" -eq 0 ]]; then
  echo "[$(log_ts)] No new workplan to process since last execution time: $LAST_EXEC_TS"
  # 更新状态为当前时间，以便下次只处理之后新增的计划
  NEW_LAST_EXEC_TS="$(date +%s)"
  if [[ "$USE_JQ" -eq 1 ]]; then
    jq -n --argjson ts "$NEW_LAST_EXEC_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
  else
    printf '{"lastExecutionTime":%s}\n' "$NEW_LAST_EXEC_TS" > "$STATE_FILE"
  fi
  exit 0
fi

echo "[$(log_ts)] Found ${#NEW_PLANS[@]} new workplan(s) to process."

# -------- 准备代码工作区：强制重新克隆 --------
echo "[$(log_ts)] Preparing code workspace: $CODE_REPO_DIR (branch: $CODE_BRANCH)"
if [[ -d "$CODE_REPO_DIR" ]]; then
  echo "[$(log_ts)] Removing existing code repo directory..."
  rm -rf "$CODE_REPO_DIR"
fi

echo "[$(log_ts)] Cloning repository: $CODE_REPO_URL"
timeout "${FETCH_TIMEOUT_SEC}" git clone --branch "$CODE_BRANCH" "$CODE_REPO_URL" "$CODE_REPO_DIR" || {
  echo "[$(log_ts)] ERROR: git clone failed." >&2
  exit 1
}

# 初始化子模块（按你的 README）
if [[ -f "$CODE_REPO_DIR/.gitmodules" ]]; then
  echo "[$(log_ts)] Initializing submodules..."
  git -C "$CODE_REPO_DIR" submodule update --init --recursive || echo "[$(log_ts)] Submodule init failed, continuing..."
fi

# 配置代码工作区的提交身份（避免 GH007 邮箱隐私问题）
if [[ -n "${GIT_USER_NAME:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.name "$GIT_USER_NAME"
fi
if [[ -n "${GIT_USER_EMAIL:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.email "$GIT_USER_EMAIL"
fi
# 避免权限位变化引起修改
git -C "$CODE_REPO_DIR" config core.filemode false

# -------- 逐个执行新排期文件 --------
MAX_TS_PROCESSED="$LAST_EXEC_TS"

for PLAN_PATH in "${NEW_PLANS[@]}"; do
  PLAN_NAME="$(basename "$PLAN_PATH")"
  PLAN_STAMP="${PLAN_NAME%.*}"
  PLAN_TS="$(plan_name_to_epoch "$PLAN_NAME")"
  if [[ "$PLAN_TS" -gt "$MAX_TS_PROCESSED" ]]; then
    MAX_TS_PROCESSED="$PLAN_TS"
  fi

  FULL_LOG_PATH="${SCRIPT_RESULT_DIR}/${PLAN_STAMP}_full.log"
  RUN_LOG_PATH="${SCRIPT_RESULT_DIR}/${PLAN_STAMP}_run.log"

  echo "[$(log_ts)] Processing plan: ${PLAN_NAME}"

  # 在代码工作区执行编译+运行（调用代码工作区的 run.sh，参数为脚本工作区的计划文件路径）
  (
    cd "$CODE_REPO_DIR"
    {
      echo "=== AutoRunner started at $(log_ts) ==="
      echo "Script workspace: ${REPO_PATH}"
      echo "Code workspace: ${CODE_REPO_DIR}, Branch: ${CODE_BRANCH}"
      echo "Workplan (script workspace): ${PLAN_PATH}"
      echo "----- Build & Run begin -----"

      # 捕获 run.sh 的“仅运行期输出”
      RUN_OUTPUT="$("$CODE_REPO_DIR/workplan/run.sh" "$PLAN_PATH" 2>&1 | tee /dev/fd/3 3>&1)"

      echo "----- Build & Run end -----"
      echo "=== AutoRunner finished at $(log_ts) ==="
    } 3> >(cat > "$RUN_LOG_PATH") | cat > "$FULL_LOG_PATH"
  ) || {
    echo "[$(log_ts)] ERROR during build/run for ${PLAN_NAME}; see full log: ${FULL_LOG_PATH}" >&2
  }

  # 将日志复制到代码工作区的 result 中，以便提交推送
  mkdir -p "$CODE_REPO_DIR/result"
  cp -f "$FULL_LOG_PATH" "$CODE_REPO_DIR/result/"
  cp -f "$RUN_LOG_PATH" "$CODE_REPO_DIR/result/"

  # 在代码工作区提交并推送（仅 result）
  (
    cd "$CODE_REPO_DIR"
    git add result || true
    git commit -m "Auto result for ${PLAN_STAMP}" || echo "Nothing to commit."
    git push || echo "Push failed. Check Git auth."
  )
done

# -------- 更新“上次执行时间”状态 --------
NEW_LAST_EXEC_TS="$MAX_TS_PROCESSED"
if [[ "$USE_JQ" -eq 1 ]]; then
  jq -n --argjson ts "$NEW_LAST_EXEC_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
else
  printf '{"lastExecutionTime":%s}\n' "$NEW_LAST_EXEC_TS" > "$STATE_FILE"
fi

echo "[$(log_ts)] All done. Updated lastExecutionTime to ${NEW_LAST_EXEC_TS}"