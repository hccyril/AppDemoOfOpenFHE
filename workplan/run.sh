#!/usr/bin/env bash
# 运行脚本（代码工作区固定路径版）
# - 工作目录固定为：$HOME/runner/AppDemoOfOpenFHE
# - 只编译并运行你的示例应用，假设 OpenFHE 已安装在 $HOME/openfhe-install
# - 接收一个参数：计划文件路径（来自“代码工作区”的 workplan/*.txt）
# - 将“仅运行期输出”打印到标准输出（供上层采集）

set -euo pipefail

WORKPLAN_FILE="${1:?Usage: run.sh <workplan_file>}"

# 固定工作目录（代码工作区）
CODE_REPO_DIR="${HOME}/runner/AppDemoOfOpenFHE"
OPENFHE_PREFIX="${HOME}/openfhe-install"
BUILD_DIR="build"
APP_BINARY="./build/AppDemo"

# 切换到代码工作区
cd "$CODE_REPO_DIR"

# 展示计划文件内容（如需要用于参数）
if [[ -f "$WORKPLAN_FILE" ]]; then
  echo "Workplan file: $WORKPLAN_FILE"
  echo "Workplan content:"
  cat "$WORKPLAN_FILE"
fi

# 验证 OpenFHE 已安装（可选，失败仅提示不退出）
if [[ ! -f "${OPENFHE_PREFIX}/lib/libOPENFHEcore.so" ]]; then
  echo "[Warn] OpenFHE not found in ${OPENFHE_PREFIX}. Please ensure it is installed." >&2
fi

# 构建应用（参考 README 第 4 节）
echo "[App] Configure & build (Release)..."
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

# 程序的标准输出即“仅运行期输出”
"${APP_BINARY}"