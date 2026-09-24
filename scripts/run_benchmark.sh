#!/usr/bin/env bash
# 默认运行 18-case 正式矩阵；验收最低矩阵使用 --minimum。
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 "$repo_dir/scripts/run_benchmark.py" "$@"
