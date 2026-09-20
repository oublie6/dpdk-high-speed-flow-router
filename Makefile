.PHONY: build check-build-env clean

# 系统 DPDK 的 pkg-config 会输出强制 include 参数。
# 旧版 cgo 默认拒绝该参数，因此只精确放行这些 token，不允许任意 CFLAGS。
export CGO_CFLAGS_ALLOW = ^(-include|rte_config\.h|-mrtm)$$

build: check-build-env
	CGO_ENABLED=1 go build -o bin/flow-router ./cmd/flow-router

check-build-env:
	@command -v go >/dev/null || { echo "Go is required" >&2; exit 1; }
	@command -v pkg-config >/dev/null || { echo "pkg-config is required" >&2; exit 1; }
	@test "$$(pkg-config --modversion libdpdk)" = 25.11.3 || { echo "DPDK 25.11.3 required: run scripts/install_dpdk.sh and check PKG_CONFIG_PATH" >&2; exit 1; }

clean:
	rm -rf bin
