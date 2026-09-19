# 架构说明：Goal 001 Bootstrap Boundary

## 1. 当前实现

Goal 001 已经完成一个同步的一次性 EAL probe：

~~~text
cmd/flow-router
-> control/dataplane.Probe(EAL args)
-> cgo
-> dp_runtime_init
-> rte_eal_init
-> dp_runtime_get_info
-> Go-owned Info snapshot
-> dp_runtime_cleanup
-> rte_eal_cleanup
-> process exit
~~~

这只是生命周期和边界验证，不是 packet forwarding，也不是性能 benchmark。

## 2. Go / C ownership

Go 当前负责：

- CLI 参数；
- 生命周期串行化；
- 返回给业务层的 `Info`；
- 错误包装；
- 触发 EAL init/info/cleanup。

只有 `control/dataplane` package 使用 cgo。

公共 C API 定义在：

~~~text
dataplane/include/dp_api.h
~~~

DPDK 的 header/type 不向 Go 业务代码扩散。

C 负责 EAL process-global state，以及后续 port / mempool / queue / worker / packet hot path。

返回给 Go 的 `Info` 是 Go 自己持有的 snapshot，不包含长期 C pointer。

## 3. argv 生命周期

`rte_eal_init()` 允许修改 argv，因此不能直接把 Go string/slice 内存交给 C 长期使用。

当前 wrapper 会：

1. 为 argv[0] 和每个 EAL 参数创建 C string；
2. 创建 C memory 中的 `char **argv`；
3. 额外保存原始 C string 地址；
4. 让 EAL 自由调整 argv pointer order；
5. cleanup 成功后按原始地址逐一 free；
6. init 或 cleanup 出现终止性失败时，让少量 C memory 随进程退出回收，避免错误释放潜在仍被部分 EAL state 引用的内存。

当前没有把 Go pointer 保存在 C 中。

## 4. OS thread 与 EAL 生命周期

Go goroutine 可以在 OS thread 之间迁移，而 EAL 会影响调用线程的 affinity 和 thread-local lcore state。

因此当前 Probe：

~~~text
Go caller
   ↓
dedicated goroutine
   ↓
runtime.LockOSThread()
   ↓
EAL init
   ↓
runtime info
   ↓
EAL cleanup
   ↓
locked goroutine exits
~~~

Goal 001 是一次性 probe，所以 locked goroutine 完成后直接退出。

未来 long-running dataplane 需要改成明确的状态机：

~~~text
Init
-> Start workers
-> Running
-> Stop request
-> Join workers
-> Release packet resources
-> EAL cleanup
~~~

worker 必须停止并释放 queue/mbuf ownership 后，才能 cleanup。

## 5. 错误模型

项目级 C API 使用：

~~~text
0           success
negative    errno-style error
~~~

Go wrapper 负责把错误转换成带 operation context 的 Go error。

EAL init 失败视为当前进程不可 retry；EAL cleanup 后同样不允许重新启动同一套 EAL 生命周期。

## 6. 当前构建方式

Goal 001 为了让 cgo 编译外部 C 实现，使用：

~~~text
control/dataplane/runtime_linux.c
    #include "../../dataplane/core/dp_runtime.c"
~~~

同时 `make build` 使用 `go build -a`，避免外部 C 文件变化没有被 Go build cache 正确感知。

这个方案只作为 bootstrap 过渡。

Goal 002 开始真正增加 `dp_port.c`、`dp_mempool.c`、`dp_worker.c` 等文件前，必须确定稳定的 native C build 方案，不能继续无限扩展 `#include *.c` 模式。

## 7. DPDK 版本约束

Goal 001 实际验证环境：

~~~text
Ubuntu 20.04
Go 1.13.8
DPDK 19.11.14
~~~

既有软件仿真实验主线已经验证：

~~~text
DPDK 25.11.3
Go + cgo
TAP PMD
PCAP PMD
mempool/mbuf labs
RTC forwarding lab
~~~

因此 Goal 002 前必须把本项目统一到 DPDK 25.11.3。

目标不是兼容 19.11 和 25.11 两套 API，而是选择一个现代基线继续开发。

## 8. 未来规则更新

未来 Go 控制面会负责：

~~~text
validate rule
-> build control command / snapshot
-> coarse cgo boundary
-> C-owned dataplane table
~~~

packet 不会进入 Go。

动态更新的 allocation/publication/reclamation 方案在实现前单独设计；Goal 001 没有实现 RCU/QSBR。

## 9. 当前仿真边界

当前只证明：

- Go/cgo/C 边界；
- EAL lifecycle；
- 软件环境构建和错误路径。

当前不能证明：

- 真实 NIC DMA；
- hardware RSS；
- cross-NUMA cost；
- line-rate throughput；
- real PMD descriptor behavior。

这些必须在后续真实硬件阶段单独验证。

## 10. 下一步

Goal 002 前先确定：

1. DPDK 25.11.3 的安装/复用方式；
2. native C dataplane 的构建组织；
3. virtual PMD 测试拓扑；
4. mempool / port / RXQ / TXQ 生命周期；
5. single-owner RTC worker 模型。
