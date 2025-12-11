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