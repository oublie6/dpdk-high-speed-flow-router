#!/usr/bin/env bash
# 只读环境检查；缺少构建必需项时返回非零。
set -u

missing=0

for tool in go gcc clang pkg-config meson ninja python3 ip; do
    if command -v "$tool" >/dev/null 2>&1; then
        if [[ "$tool" == go ]]; then
            go version
        elif [[ "$tool" == ip ]]; then
            ip -Version
        else
            "$tool" --version 2>/dev/null | head -n 1
        fi
    else
        printf '%s: unavailable\n' "$tool"
        if [[ "$tool" != gcc && "$tool" != clang ]]; then
            missing=1
        fi
    fi
done

if command -v go >/dev/null 2>&1; then
    printf 'Process CGO_CFLAGS_ALLOW: %s\n' "${CGO_CFLAGS_ALLOW:-not set; Makefile supplies the project value}"
fi

if ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    echo 'A C compiler (gcc or clang) is required.' >&2
    missing=1
fi

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdpdk; then
    actual=$(pkg-config --modversion libdpdk)
    printf 'DPDK version: %s\n' "$actual"
    if [[ "$actual" != 25.11.3 ]]; then
        echo '本项目要求 DPDK 精确为 25.11.3；请先执行 scripts/install_dpdk.sh 并检查 PKG_CONFIG_PATH。' >&2
        missing=1
    fi
    printf 'DPDK prefix: %s\n' "$(pkg-config --variable=prefix libdpdk)"
    printf 'DPDK pkg-config directory: %s\n' "$(pkg-config --variable=pcfiledir libdpdk)"
    printf 'DPDK cflags: '
    pkg-config --cflags libdpdk
    printf 'DPDK libs: '
    pkg-config --libs libdpdk
else
    echo 'libdpdk unavailable: install DPDK development files or set PKG_CONFIG_PATH.' >&2
    missing=1
fi

printf '\nTUN/TAP device:\n'
if [[ -c /dev/net/tun ]]; then
    ls -l /dev/net/tun
else
    echo '/dev/net/tun 不存在；TAP 验证需要此设备。' >&2
    missing=1
fi
printf '\nDistro / kernel:\n'
if [[ -r /etc/os-release ]]; then cat /etc/os-release; fi
uname -r

printf '\nHugePage state:\n'
if [[ -r /proc/meminfo ]]; then
    awk '/HugePages|Hugepagesize|Hugetlb/ {print}' /proc/meminfo
else
    echo '/proc/meminfo unavailable'
fi

printf '\nCPU / allowed affinity (EAL lcores must map to allowed CPUs):\n'
if command -v lscpu >/dev/null 2>&1; then
    lscpu
fi
if [[ -r /proc/self/status ]]; then
    awk '/Cpus_allowed_list/ {print}' /proc/self/status
fi

printf '\nNUMA topology:\n'
if command -v numactl >/dev/null 2>&1; then
    numactl --hardware
elif [[ -r /sys/devices/system/node/online ]]; then
    printf 'Online nodes: '
    cat /sys/devices/system/node/online
else
    echo 'NUMA information unavailable'
fi

exit "$missing"
