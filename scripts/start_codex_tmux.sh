#!/usr/bin/env bash
# start_codex_tmux.sh 用于在服务器上安装 tmux，并在当前项目根目录
# 启动或连接本仓库专用的 Codex tmux session。
set -euo pipefail

NO_ATTACH=0
TMUX_CONF="${HOME}/.tmux.conf"
TMUX_COLOR_BLOCK_BEGIN="# >>> start-codex-tmux 颜色配置 >>>"
TMUX_COLOR_BLOCK_END="# <<< start-codex-tmux 颜色配置 <<<"
CODEX_CMD="env TERM=screen-256color COLORTERM=truecolor codex --no-alt-screen --dangerously-bypass-approvals-and-sandbox"

err() {
  echo "Error: $*" >&2
}

info() {
  echo "==> $*"
}

need_sudo() {
  if [[ "${EUID}" -eq 0 ]]; then
    return 1
  fi
  return 0
}

run_as_root() {
  if need_sudo; then
    if ! command -v sudo >/dev/null 2>&1; then
      err "需要 root 权限安装 tmux，但当前用户不是 root，且系统中找不到 sudo。"
      exit 1
    fi
    sudo "$@"
  else
    "$@"
  fi
}

install_tmux() {
  info "tmux 未安装，开始自动安装。"

  if command -v apt-get >/dev/null 2>&1; then
    run_as_root apt-get update
    run_as_root apt-get install -y tmux
  elif command -v apt >/dev/null 2>&1; then
    run_as_root apt update
    run_as_root apt install -y tmux
  elif command -v dnf >/dev/null 2>&1; then
    run_as_root dnf install -y tmux
  elif command -v yum >/dev/null 2>&1; then
    run_as_root yum install -y tmux
  elif command -v apk >/dev/null 2>&1; then
    run_as_root apk add --no-cache tmux
  else
    err "找不到支持的包管理器，无法自动安装 tmux。请手动安装 tmux 后重试。"
    err "当前脚本支持的包管理器：apt/apt-get、dnf、yum、apk。"
    exit 1
  fi
}

ensure_tmux_config() {
  local temp_conf

  if [[ ! -f "${TMUX_CONF}" ]]; then
    touch "${TMUX_CONF}"
  fi

  temp_conf=$(mktemp "${TMUX_CONF}.tmp.XXXXXX")
  awk -v begin="${TMUX_COLOR_BLOCK_BEGIN}" -v end="${TMUX_COLOR_BLOCK_END}" '
    $0 == begin { in_managed_block = 1; next }
    $0 == end { in_managed_block = 0; next }
    !in_managed_block { print }
  ' "${TMUX_CONF}" >"${temp_conf}"

  {
    printf '%s\n' "${TMUX_COLOR_BLOCK_BEGIN}"
    printf '%s\n' 'set -g default-terminal "screen-256color"'
    printf '%s\n' "set -g terminal-overrides '*:RGB,xterm*:smcup@:rmcup@'"
    printf '%s\n' 'set -g history-limit 500000'
    printf '%s\n' 'set -g mouse on'
    printf '%s\n' 'bind-key u copy-mode -e'
    printf '%s\n' "${TMUX_COLOR_BLOCK_END}"
  } >>"${temp_conf}"

  chmod --reference="${TMUX_CONF}" "${temp_conf}" 2>/dev/null || true
  mv "${temp_conf}" "${TMUX_CONF}"
  tmux source-file "${TMUX_CONF}" 2>/dev/null || true
}

usage() {
  cat <<EOF
用法: $0 [--no-attach]

选项:
  --no-attach  只创建或确认 tmux session 存在，不 attach 到终端。

环境变量:
  CODEX_TMUX_SESSION  自定义 session 名称。
                      默认: codex-dpdk-high-speed-flow-router
EOF
}

for arg in "$@"; do
  case "${arg}" in
    --no-attach)
      NO_ATTACH=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      err "未知参数：${arg}"
      usage >&2
      exit 1
      ;;
  esac
done

if ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null); then
  :
else
  ROOT_DIR=$(pwd)
fi

SESSION_NAME="${CODEX_TMUX_SESSION:-codex-dpdk-high-speed-flow-router}"

if ! command -v tmux >/dev/null 2>&1; then
  install_tmux
fi

if ! command -v tmux >/dev/null 2>&1; then
  err "tmux 安装后仍不可用，请检查包管理器输出或手动安装 tmux。"
  exit 1
fi

ensure_tmux_config

if ! command -v codex >/dev/null 2>&1; then
  err "找不到 codex 命令。请先安装 Codex CLI，并确保 codex 在 PATH 中。"
  exit 1
fi

if ! tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
  info "创建 tmux session：${SESSION_NAME}"
  info "工作目录：${ROOT_DIR}"
  tmux new-session -d -s "${SESSION_NAME}" -c "${ROOT_DIR}" "${CODEX_CMD}"
else
  info "tmux session 已存在，不重复创建或启动 codex：${SESSION_NAME}"
fi

if [[ "${NO_ATTACH}" -eq 1 ]]; then
  info "--no-attach 已启用，session 已准备好：${SESSION_NAME}"
  exit 0
fi

exec tmux attach-session -t "${SESSION_NAME}"
