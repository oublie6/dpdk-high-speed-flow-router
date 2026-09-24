#!/usr/bin/env bash
# Python 负责精确 frame、worker distribution 断言与 finally cleanup。
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 "$repo_dir/scripts/verify_multiqueue.py" "$@"
