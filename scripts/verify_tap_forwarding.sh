#!/usr/bin/env bash
# Python 负责整体 deadline、精确抓包断言和 finally 清理；本脚本不修改主机网络。
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 "$repo_dir/scripts/verify_tap_forwarding.py" "$@"
