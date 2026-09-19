.PHONY: build check-build-env clean

# System DPDK pkg-config emits this forced config include. Older cgo versions
# reject it by default; allow only these exact tokens, not arbitrary flags.
export CGO_CFLAGS_ALLOW = -include|rte_config.h

# The C implementation/header live outside the cgo package; bypass the Go
# cache so changes there are always compiled.
build: check-build-env
	CGO_ENABLED=1 go build -a -o bin/flow-router ./cmd/flow-router

check-build-env:
	@command -v go >/dev/null || { echo "Go is required" >&2; exit 1; }
	@command -v pkg-config >/dev/null || { echo "pkg-config is required" >&2; exit 1; }
	@pkg-config --exists libdpdk || { echo "DPDK development files missing: install libdpdk-dev or set PKG_CONFIG_PATH" >&2; exit 1; }

clean:
	rm -rf bin
