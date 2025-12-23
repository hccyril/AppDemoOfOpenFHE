#!/usr/bin/env bash
# AutoRunner（简化版：先删后克隆，在代码工作区扫描排期并执行）
# 说明：
# - 始终先删除代码工作区目录，等待3秒，再克隆最新代码到代码工作区
# - 仅在“代码工作区”的 workplan 中检查新增排期（文件名形如 YYYYMMDD_HHMM.txt）
# - 编译与运行在“代码工作区”执行
# - 日志与结果首先写入“脚本工作区”的 result 目录，然后复制一份到“代码工作区”的 result
# - 提交与推送在“代码工作区”执行（仅提交 result 目录）
# - 假定 jq 已安装可用

set -euo pipefail

# 脚本工作区（当前仓库）
REPO_PATH="${REPO_PATH:-$PWD}"
SCRIPT_RESULT_DIR="${SCRIPT_RESULT_DIR:-$REPO_PATH/result}"
STATE_FILE="${STATE_FILE:-$REPO_PATH/scripts/state.json}"

# 代码工作区
CODE_WORKSPACE="${CODE_WORKSPACE:-$HOME/runner}"
CODE_REPO_DIR="${CODE_WORKSPACE}/AppDemoOfOpenFHE"

# 仓库与分支
DEFAULT_REPO_URL="git@github.com:hccyril/AppDemoOfOpenFHE.git"
CODE_REPO_URL="${CODE_REPO_URL:-$(git -C "$REPO_PATH" config --get remote.origin.url 2>/dev/null || echo "$DEFAULT_REPO_URL")}"
CODE_BRANCH="${CODE_BRANCH:-main}"

# 超时
FETCH_TIMEOUT_SEC="${FETCH_TIMEOUT_SEC:-60}"

# 可选：提交身份（建议设置为 GitHub noreply 邮箱）
# export GIT_USER_NAME="hccyril"
# export GIT_USER_EMAIL="12345678+hccyril@users.noreply.github.com"

log_ts() { date +"%Y-%m-%d %H:%M:%S"; }

# 目录准备
mkdir -p "$SCRIPT_RESULT_DIR" "$(dirname "$STATE_FILE")" "$CODE_WORKSPACE"

# 读取上次执行时间（秒）；如无，默认“当前时间 - 3600 秒”
LAST_EXEC_TS=""
if [[ -f "$STATE_FILE" ]]; then
  LAST_EXEC_TS="$(jq -r '.lastExecutionTime // empty' "$STATE_FILE")"
fi
if [[ -z "${LAST_EXEC_TS:-}" ]]; then
  LAST_EXEC_TS=$(( $(date +%s) - 3600 ))
fi

# 将计划文件名转换为时间戳；期望格式：YYYYMMDD_HHMM.txt
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

# 删除代码工作区（若存在），等待3秒，再克隆
if [[ -d "$CODE_REPO_DIR" ]]; then
  echo "[$(log_ts)] Removing existing code repo directory..."
  rm -rf "$CODE_REPO_DIR"
  sleep 3
fi

echo "[$(log_ts)] Cloning repository: $CODE_REPO_URL"
timeout "${FETCH_TIMEOUT_SEC}" git clone --branch "$CODE_BRANCH" "$CODE_REPO_URL" "$CODE_REPO_DIR" || {
  echo "[$(log_ts)] ERROR: git clone failed." >&2
  exit 1
}

# 在代码工作区设置提交身份（如提供）与 filemode
if [[ -n "${GIT_USER_NAME:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.name "$GIT_USER_NAME"
fi
if [[ -n "${GIT_USER_EMAIL:-}" ]]; then
  git -C "$CODE_REPO_DIR" config user.email "$GIT_USER_EMAIL"
fi
git -C "$CODE_REPO_DIR" config core.filemode false

CLONED_HEAD="$(git -C "$CODE_REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"

# 在代码工作区扫描新增排期
CODE_PLAN_DIR="$CODE_REPO_DIR/workplan"
if [[ ! -d "$CODE_PLAN_DIR" ]]; then
  echo "[$(log_ts)] No workplan directory found in code workspace: $CODE_PLAN_DIR"
  NOW_TS="$(date +%s)"
  jq -n --argjson ts "$NOW_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
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
  jq -n --argjson ts "$NOW_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"
  exit 0
fi

echo "[$(log_ts)] Found ${#NEW_PLANS[@]} new workplan(s)."

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

      if [[ -x "$CODE_REPO_DIR/workplan/run.sh" ]]; then
        RUN_OUTPUT="$("$CODE_REPO_DIR/workplan/run.sh" "$PLAN_FILE" 2>&1 | tee /dev/fd/3 3>&1)"
      else
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
    git push origin "$CODE_BRANCH" || echo "Push failed. Check Git auth."
  )
done

# 更新 lastExecutionTime
NEW_LAST_EXEC_TS="$MAX_TS_PROCESSED"
jq -n --argjson ts "$NEW_LAST_EXEC_TS" '{lastExecutionTime: $ts}' > "$STATE_FILE"

echo "[$(log_ts)] All done. Updated lastExecutionTime to ${NEW_LAST_EXEC_TS}"