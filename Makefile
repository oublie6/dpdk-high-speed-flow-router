.PHONY: build check-build-env clean

# 系统 DPDK 的 pkg-config 会输出强制 include 参数。
# 旧版 cgo 默认拒绝该参数，因此只精确放行这两个 token，不允许任意 CFLAGS。
export CGO_CFLAGS_ALLOW = -include|rte_config.h

# Goal 001 的 C 实现位于 cgo package 外部。
# 当前暂时使用 -a 避免外部 C 文件变化被 Go build cache 漏掉；
# Goal 002 前会收敛为稳定的 native C 构建方式。
build: check-build-env
	CGO_ENABLED=1 go build -a -o bin/flow-router ./cmd/flow-router

check-build-env:
	@command -v go >/dev/null || { echo "Go is required" >&2; exit 1; }
	@command -v pkg-config >/dev/null || { echo "pkg-config is required" >&2; exit 1; }
	@pkg-config --exists libdpdk || { echo "DPDK development files missing: install libdpdk-dev or set PKG_CONFIG_PATH" >&2; exit 1; }

clean:
	rm -rf bin
