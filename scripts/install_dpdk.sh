#!/usr/bin/env bash
# 只安装软件构建基线；不操作 PCI、网络、内核参数或 HugePage。
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo '请以 root 或 sudo 执行安装。' >&2; exit 1; }
version=25.11.3
source_dir=/opt/src/dpdk-25.11.3
archive=/opt/src/dpdk-25.11.3.tar.xz
tools_dir=/opt/dpdk-build-tools
checksum=3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490

# 固定工具版本，避免 Ubuntu 20.04 的旧 Meson 不满足 DPDK 要求。
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config python3 python3-venv python3-pip ninja-build \
    libnuma-dev libelf-dev libssl-dev zlib1g-dev libbpf-dev \
    curl ca-certificates xz-utils iproute2
if [[ ! -x "$tools_dir/bin/python" ]]; then
    python3 -m venv "$tools_dir"
fi
"$tools_dir/bin/python" -m pip install 'meson==1.5.2' 'pyelftools==0.32'
ln -sfn "$tools_dir/bin/meson" /usr/local/bin/meson
# Meson 查找的 python3 也必须来自同一个 venv，才能导入 elftools。
export PATH="$tools_dir/bin:$PATH"

mkdir -p /opt/src
if [[ ! -f "$archive" ]]; then
    # 临时文件只在完整下载、校验后成为缓存，失败不保留半个压缩包。
    partial=$(mktemp /opt/src/dpdk-download.XXXXXX)
    trap 'rm -f "$partial"' EXIT
    curl -fL --retry 3 --connect-timeout 20 --max-time 600 \
        "https://fast.dpdk.org/rel/dpdk-${version}.tar.xz" -o "$partial"
    printf '%s  %s\n' "$checksum" "$partial" | sha256sum -c -
    mv "$partial" "$archive"
    trap - EXIT
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
if [[ ! -d "$source_dir" ]]; then
    mkdir "$source_dir"
    tar -xJf "$archive" -C "$source_dir" --strip-components=1
fi
[[ $(cat "$source_dir/VERSION") == "$version" ]] || {
    echo '源码版本不匹配；保留已有目录，请人工检查。' >&2; exit 1;
}

# 仅构建本 Goal 需要的虚拟设备和 ring mempool driver，不引入真实 NIC driver。
# /usr/local/lib 是标准 loader/pkg-config 搜索目录，系统 19.11 package 可保留。
setup_args=(--prefix=/usr/local --libdir=lib --buildtype=release
    -Dplatform=generic -Ddefault_library=shared -Dtests=false -Dexamples=
    -Denable_drivers=bus/vdev,mempool/ring,net/tap)
if [[ -f "$source_dir/build/build.ninja" ]]; then
    "$tools_dir/bin/meson" setup --reconfigure "$source_dir/build" "$source_dir" "${setup_args[@]}"
else
    "$tools_dir/bin/meson" setup "$source_dir/build" "$source_dir" "${setup_args[@]}"
fi
ninja -C "$source_dir/build" -j "${DPDK_BUILD_JOBS:-4}"
ninja -C "$source_dir/build" install
ldconfig
actual=$(pkg-config --modversion libdpdk)
[[ "$actual" == "$version" ]] || {
    echo "DPDK 解析为 $actual；请检查 PKG_CONFIG_PATH 是否覆盖 /usr/local/lib/pkgconfig。" >&2
    exit 1
}
printf 'DPDK version: %s\npkg-config directory: %s\n' "$actual" "$(pkg-config --variable=pcfiledir libdpdk)"
