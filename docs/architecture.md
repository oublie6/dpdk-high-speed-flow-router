# Goal 001: bootstrap boundary

Implemented: a Linux Go CLI performs one synchronous EAL probe through a small
cgo package and a project C API. This is lifecycle evidence, not a forwarding
implementation or performance benchmark. The rest of v0.1 remains planned.

```
cmd/flow-router -> control/dataplane.Probe(EAL arguments)
               -> dp_runtime_init -> rte_eal_init
               -> dp_runtime_get_info -> Go-owned snapshot
               -> dp_runtime_cleanup -> rte_eal_cleanup
               -> process exit
```

## Ownership and lifetime

Go owns CLI arguments, lifecycle serialization and the returned `Info` value.
Only `control/dataplane` imports C. The public header is
`dataplane/include/dp_api.h`; DPDK headers/types do not escape into Go business
code. `runtime_linux.c` is a cgo compilation adapter for the implementation in
`dataplane/core`, not another runtime implementation. Since those C sources/headers live outside
the Go package, `make build` uses `go build -a` to avoid stale cgo cache entries
after C changes. Use `go test -a` or clear the build cache when testing C edits.

Build correction: initially plain `#cgo pkg-config: libdpdk` was expected to
suffice. The installed DPDK 19.11.14 emits `-include rte_config.h`, rejected by
Go 1.13.8's cgo flag filter. The Makefile and documented direct Go commands now
allow only those two exact tokens via `CGO_CFLAGS_ALLOW`; all include/library
paths still come from pkg-config. This preserves the system DPDK build contract
without accepting arbitrary compiler flags.

The wrapper allocates mutable argv and strings in C memory, including argv[0]
and a trailing null pointer. It retains original string addresses separately
because EAL can reorder the pointer array. No Go pointers are retained in C.
Successful cleanup frees the original strings and array exactly once. On failed
init or failed cleanup, the process must exit: these small C allocations are
intentionally retained until exit in case partially initialized EAL state still
references them. There is no retry in the same process.

C owns EAL state. Runtime info borrows DPDK's static version string only until
Go copies it before cleanup; the returned Go snapshot has no C pointers.
Project API success is zero; failures are negative errno values, converted to
Go errors with operation context. Calling info/cleanup before initialization
fails. Reinitialization is rejected, including after successful cleanup.

The mutable argv contract and one-init restriction follow the
[DPDK EAL API](https://doc.dpdk.org/api-19.11/rte__eal_8h.html).

## Thread and lifecycle model

Assumption: consecutive cgo calls could simply run from the CLI goroutine.
Problem: Go can migrate goroutines between OS threads, while EAL sets calling
thread affinity and thread-local lcore state. Design: serialize the lifecycle
and execute init/info/cleanup in a dedicated goroutine locked to one OS thread.
The goroutine exits while locked so Go retires that thread; its EAL affinity
must not leak back into the general Go scheduler. Trade-off: one thread for a
one-shot probe, with no long-running worker manager yet.

`Probe` returns only after cleanup, so CLI success messages describe the
completed lifecycle. If info retrieval fails after successful init, cleanup is
still attempted. Init failure returns directly with a nonzero CLI exit. EAL
help options may terminate inside DPDK; they are not a successful probe.
The CLI requires `--` and passes subsequent arguments verbatim; there is no
configuration file or implicit host-specific CPU/NIC selection.

## Future paths (not implemented)

A future long-lived runtime must preserve a dedicated EAL thread and define
start/stop/join before cleanup. C workers will own queues, mbufs and packet
processing; Go will request lifecycle transitions through coarse APIs. Worker
shutdown must complete and owned resources must be released before EAL cleanup.
No per-packet Go callbacks are permitted.

Future rule updates will be validated in Go and published as C-owned snapshots
through a coarse API. The allocation, publication and retirement contract must
be agreed before implementing updates; no Go pointers may escape into worker
state. Flow/route semantics, rewrite fields, software topology, queue/lcore
mapping and benchmark baseline remain open. RCU/QSBR is not implemented here.

Next gate: review Goal 001's commit, boundary and runtime evidence, then agree
on Goal 002 (mempool, virtual PMD, single RXQ/TXQ, RTC forwarding). No NIC
binding, network changes, real-NIC/NUMA claims or benchmark results are part of
this milestone.
