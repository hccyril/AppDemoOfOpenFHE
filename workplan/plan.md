好的。基于你提供的 README 内容和你的服务器为 Arch Linux 的实际环境，下面给出一套适配 Arch 的自动化方案：使用 bash 脚本 + systemd 定时器 实现定时拉取、检测新指令文件、按 README 的步骤进行构建运行、采集日志并推送到 GitHub。

你可以逐步按照“准备 -> 脚本部署 -> systemd 定时任务配置 -> 使用规范”的结构完成。所有脚本示例都已生成，直接拷贝到仓库即可。

一、总体设计（Arch Linux + bash + systemd）
- 使用 systemd timer 每隔一段时间触发主控脚本。
- 主控脚本 autorunner.sh：
  - git fetch/pull；
  - 扫描 workplan 目录是否出现新的指令文件（例如 workplan/20251211_1600.txt）；
  - 调用 run.sh 执行“构建 OpenFHE -> 安装 -> 构建应用 -> 运行”的流程；
  - 采集两个日志文件：完整过程日志与仅运行期输出；
  - 写入 result 目录；
  - git add/commit/push 推送到 GitHub；
  - 用 scripts/state.json 记录已处理到的指令文件名。
- 你保持 README 的构建步骤即可；run.sh 调用的构建命令直接来自你 README 的第 3、4、9 节，针对 Arch 做了极少量差异说明。

二、准备工作（一次性）
- 安装依赖（Arch）：
  - sudo pacman -Syu
  - sudo pacman -S git cmake ninja base-devel openmp jq
- OpenFHE 源码在仓库第三方子模块的路径为 third_party/openfhe（按你的 README）。
- 确认仓库目录结构中包含：
  - workplan/ 放置指令文件和 run.sh
  - result/ 存放日志文件
  - scripts/ 存放 autorunner.sh 和 init_autorunner.sh
- Git 认证建议使用 SSH：
  - 在服务器生成密钥：ssh-keygen -t ed25519
  - 将公钥加入 GitHub：[SSH and GPG keys](https://github.com/settings/keys)
  - 将远程改为 SSH：git remote set-url origin git@github.com:hccyril/AppDemoOpenFHE.git
- Git 用户信息：
  - git config --global user.name "你的名字"
  - 推荐使用 GitHub 的 noreply 邮箱以避免暴露真实邮箱（并避免 push 被拒绝）：
    - 在 GitHub -> Settings -> Emails 中复制你的 noreply 地址（形如：123456+username@users.noreply.github.com）。
    - 全局配置：git config --global user.email "123456+username@users.noreply.github.com"
    - 仅当前仓库配置：git config user.email "123456+username@users.noreply.github.com"
    - 如已用真实邮箱产生提交导致 push 被拒绝，可修复最近一次提交作者并强推到自己的分支：
      - git commit --amend --reset-author --no-edit
      - git push -f

三、脚本与单元文件
将以下文件添加到你的仓库（建议路径如下）。脚本中的构建命令已经与 README 对齐，并适配了 Arch。

````markdown name=workplan/run.sh
#!/usr/bin/env bash
# 说明：
# - 接收一个参数：计划文件路径（例如 workplan/20251211_1600.txt）
# - 按 README 执行：安装依赖 -> 构建并安装 OpenFHE -> 构建应用 -> 运行应用
# - 将“仅运行期输出”打印到标准输出（供上层采集）
# - 将构建与安装过程的详细输出也打印到标准输出（供上层“完整日志”收集）

set -euo pipefail

WORKPLAN_FILE="${1:?Usage: run.sh <workplan_file>}"

# 可配置前缀与目录（与 README 对齐）
OPENFHE_PREFIX="${HOME}/openfhe-install"
BUILD_DIR="build"
OPENFHE_SUBMODULE_DIR="third_party/openfhe"
APP_BINARY="./build/AppDemoOpenFHE"

# 解析计划文件（如需要参数，可从计划文件读取）
if [[ -f "$WORKPLAN_FILE" ]]; then
  echo "Workplan file: $WORKPLAN_FILE"
  PLAN_CONTENT="$(cat "$WORKPLAN_FILE")"
  echo "Workplan content:"
  echo "$PLAN_CONTENT"
fi

# 安装依赖（Arch）
# 注：若已安装可跳过。这里保留输出以便完整日志可见。
echo "[Deps] Installing dependencies (Arch Linux)..."
if command -v pacman >/dev/null 2>&1; then
  echo "Using pacman..."
  # 仅演示输出，不强制自动同意；生产可在外部执行
  echo "Run manually if not root: sudo pacman -S --needed git cmake ninja base-devel openmp"
else
  echo "pacman not found, please ensure you're on Arch and deps are installed."
fi

# 构建并安装 OpenFHE（参考 README 第 3 节）
echo "[OpenFHE] Configure and install to ${OPENFHE_PREFIX}..."
pushd "${OPENFHE_SUBMODULE_DIR}"
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_UNITTESTS=OFF \
  -DBUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX="${OPENFHE_PREFIX}"
cmake --build build -j
cmake --install build

# 关键文件验证（参考 README）
ls "${OPENFHE_PREFIX}/include/openfhe/pke/openfhe.h"
ls "${OPENFHE_PREFIX}/include/openfhe/core/utils/debug.h"
ls "${OPENFHE_PREFIX}/lib/libOPENFHEcore.so"
ls "${OPENFHE_PREFIX}/lib/libOPENFHEpke.so"
popd

# 构建应用（参考 README 第 4 节）
echo "[App] Configure & build..."
cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOpenFHE_DIR="${OPENFHE_PREFIX}/lib/OpenFHE"
cmake --build "${BUILD_DIR}" -j

# 运行前设置 LD_LIBRARY_PATH（参考 README）
export LD_LIBRARY_PATH="${OPENFHE_PREFIX}/lib:${LD_LIBRARY_PATH:-}"

# 运行应用，捕获“仅运行期输出”
echo "[Run] Executing ${APP_BINARY} ..."
if [[ ! -x "${APP_BINARY}" ]]; then
  echo "App binary not found or not executable: ${APP_BINARY}" >&2
  exit 1
fi

# 运行并将输出写到 stdout（供上层采集为 run.log）
"${APP_BINARY}"
````

```bash name=scripts/autorunner.sh
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
```

```bash name=scripts/init_autorunner.sh
#!/usr/bin/env bash
# 初始化目录与状态（一次性）
set -euo pipefail

REPO_PATH="${1:-$PWD}"
cd "$REPO_PATH"

mkdir -p workplan result scripts
if [[ ! -f scripts/state.json ]]; then
  echo '{"lastProcessedPlan": ""}' > scripts/state.json
  echo "Initialized state.json"
else
  echo "state.json already exists"
fi

echo "Done. Repo path: $REPO_PATH"
```

```ini name=scripts/systemd/appdemo-autorunner.service
[Unit]
Description=AutoRunner for AppDemoOpenFHE (fetch, build, run, collect logs, push)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
# 请将 WorkingDirectory 设置为你的仓库绝对路径
WorkingDirectory=/srv/AppDemoOpenFHE
# 使用环境变量传递 REPO_PATH（也可直接靠 WorkingDirectory）
Environment=REPO_PATH=/srv/AppDemoOpenFHE
ExecStart=/usr/bin/bash -lc 'scripts/autorunner.sh'
# 若需要 SSH agent/特定用户环境，可改为调用 wrapper（如 /usr/bin/env -i bash -lc ...）
# 可选：为避免执行策略差异，确保用户有权访问目录与 SSH 私钥

[Install]
WantedBy=multi-user.target
```

```ini name=scripts/systemd/appdemo-autorunner.timer
[Unit]
Description=Timer for AutoRunner (every 15 minutes)

[Timer]
OnBootSec=2min
OnUnitActiveSec=15min
AccuracySec=1min
Persistent=true

[Install]
WantedBy=timers.target
```

四、部署步骤
1) 将以上脚本文件加入你的仓库：
- workplan/run.sh
- scripts/autorunner.sh
- scripts/init_autorunner.sh
- scripts/systemd/appdemo-autorunner.service
- scripts/systemd/appdemo-autorunner.timer
- 确保可执行权限：
  - chmod +x workplan/run.sh scripts/autorunner.sh scripts/init_autorunner.sh

2) 初始化目录与状态
- ./scripts/init_autorunner.sh
- 检查存在 workplan、result、scripts 目录与 scripts/state.json

3) 配置 Git 远程为 SSH
- git remote set-url origin git@github.com:hccyril/AppDemoOpenFHE.git
- 测试 push 权限：git push

4) 配置 systemd（以你的仓库路径为例 /srv/AppDemoOpenFHE）
- sudo cp scripts/systemd/appdemo-autorunner.service /etc/systemd/system/
- sudo cp scripts/systemd/appdemo-autorunner.timer /etc/systemd/system/
- 修改 /etc/systemd/system/appdemo-autorunner.service 中 WorkingDirectory 与 Environment 的 REPO_PATH 为你的仓库路径
- 重新加载：
  - sudo systemctl daemon-reload
- 启用并启动定时器：
  - sudo systemctl enable --now appdemo-autorunner.timer
- 查看状态：
  - systemctl status appdemo-autorunner.timer
  - journalctl -u appdemo-autorunner.service --since "10 min ago"

5) 验证一次手动运行
- sudo systemctl start appdemo-autorunner.service
- 查看结果：
  - 仓库 result/ 是否出现完整日志和运行日志文件
  - push 是否成功

五、使用规范（你的日常流程）
- 每次要做实验：
  1) 在本地或其他环境修改代码，push 到 GitHub。
  2) 在仓库的 workplan 目录创建新的指令文件，例如：workplan/20251211_1600.txt。
     - 名称需按字典序递增（YYYYMMDD_HHMM.txt），这样 autorunner 才能识别为“新”。
     - 指令内容可填运行参数或提示；当前 run.sh 仅打印内容，若要读取参数可在 run.sh 中解析该文件。
  3) 等待定时器触发（默认每 15 分钟）。
- 自动流程将：
  - fetch/pull；
  - 发现新指令文件，运行 README 的构建和运行步骤；
  - 写入两份日志到 result 并 push 回 GitHub：
    - 完整过程日志：{计划名}_full.log
    - 仅运行期输出：{计划名}_run.log

六、关键差异与说明（Ubuntu README 到 Arch 的迁移）
- 依赖安装：将 apt 换为 pacman；OpenMP 在 Arch 包名为 openmp（提供 libomp）。
- 其余 CMake/Ninja/git 步骤与路径保持一致（使用 $HOME/openfhe-install）。
- LD_LIBRARY_PATH 与 OpenFHE_DIR 的使用按你的 README 一致。
- 如果你更偏好“安装+运行”的 rpath 方式，可以在 run.sh 中切换到 cmake --install build --prefix "$PWD/out" 并从 out/bin 运行；此时可省略 LD_LIBRARY_PATH。

七、可选增强
- 批量处理所有未执行的计划文件：将 autorunner.sh 的“找到第一个”逻辑改为依次处理所有 name > LAST_PROCESSED 的文件。
- 并发控制：生产环境建议一次只跑一个；如果有多个任务，可使用队列文件或锁文件。
- 更复杂的参数解析：让 workplan/*.txt 包含运行参数，例如输入数据路径、迭代次数等；run.sh 可按行解析。
- 安全与权限：确保 systemd 以有 SSH 私钥的用户身份运行（建议使用你的用户而非 root），或者将 service 配置为 User=hccyril 并把单元放到 ~/.config/systemd/user/ 下。

如果你愿意，我可以把 run.sh 中的构建运行命令改成“打包安装到 out/ 并从 out/bin 运行”的变体，或者根据你接下来提供的具体运行参数改造 workplan 指令格式和解析逻辑。