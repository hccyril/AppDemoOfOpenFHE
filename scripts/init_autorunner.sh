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