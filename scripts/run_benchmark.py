#!/usr/bin/env python3
"""运行 software TAP/kernel/raw-socket benchmark 并生成 CSV/Markdown。"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import traceback
import uuid


FIELDS = [
    "git_commit", "worktree_dirty", "dpdk_version", "kernel", "cpu_model",
    "allowed_cpu_list", "numa_topology", "pmd", "workers", "lcore_map",
    "rx_queues", "tx_queues", "burst_size", "packet_size", "flow_count",
    "warmup_seconds", "measure_seconds", "duration", "offered_packets",
    "offered_pps", "rx_packets", "tx_packets", "captured_packets",
    "drop_packets", "rx_mpps", "tx_mpps", "gbps", "process_cpu_seconds",
    "cpu_utilization", "worker_distribution", "observed_bottleneck",
]
MARKER = b"DFRBENCH"
SRC_IP = "192.0.2.50"
DST_IP = "198.51.100.60"


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def build_frame(packet_size, flow_id):
    payload_len = packet_size - 14 - 20 - 8
    if payload_len < len(MARKER):
        raise ValueError("packet size 太小")
    payload = MARKER + struct.pack("!I", flow_id) + bytes(payload_len - len(MARKER) - 4)
    src = socket.inet_aton(SRC_IP)
    dst = socket.inet_aton(DST_IP)
    src_port = 10000 + flow_id
    dst_port = 30000 + flow_id
    udp_len = 8 + payload_len
    ip_len = 20 + udp_len
    ip0 = struct.pack("!BBHHHBBH4s4s", 0x45, 0, ip_len, flow_id & 0xffff,
                      0, 64, socket.IPPROTO_UDP, 0, src, dst)
    ip_header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, ip_len,
                            flow_id & 0xffff, 0, 64, socket.IPPROTO_UDP,
                            checksum(ip0), src, dst)
    udp0 = struct.pack("!HHHH", src_port, dst_port, udp_len, 0)
    pseudo = src + dst + struct.pack("!BBH", 0, socket.IPPROTO_UDP, udp_len)
    udp_sum = checksum(pseudo + udp0 + payload)
    if udp_sum == 0:
        udp_sum = 0xffff
    udp_header = struct.pack("!HHHH", src_port, dst_port, udp_len, udp_sum)
    return bytes.fromhex("0200000000020200000000010800") + ip_header + udp_header + payload


def make_rules(flow_count):
    flows = []
    for flow_id in range(flow_count):
        flows.append({
            "src_ipv4": SRC_IP, "dst_ipv4": DST_IP,
            "src_port": 10000 + flow_id, "dst_port": 30000 + flow_id,
            "protocol": "udp", "action": {"type": "forward"},
        })
    return {"flows": flows, "routes": []}


def process_cpu_seconds(pid):
    fields = (Path("/proc") / str(pid) / "stat").read_text().split()
    return (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")


def environment(root):
    def output(command):
        return subprocess.check_output(command, cwd=str(root), text=True).strip()

    cpu_model = "unknown"
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break
    nodes = sorted(path.name.replace("node", "") for path in
                   Path("/sys/devices/system/node").glob("node[0-9]*"))
    return {
        "git_commit": output(["git", "rev-parse", "HEAD"]),
        "worktree_dirty": "true" if output(["git", "status", "--porcelain"]) else "false",
        "dpdk_version": output(["pkg-config", "--modversion", "libdpdk"]),
        "kernel": output(["uname", "-r"]),
        "cpu_model": cpu_model,
        "allowed_cpu_list": ",".join(map(str, sorted(os.sched_getaffinity(0)))),
        "numa_topology": "nodes=" + ",".join(nodes),
        "pmd": "net_tap (software TAP/kernel/raw-socket)",
    }


def parse_stats(log_text):
    aggregate = re.search(
        r"stats: rx=(\d+) parse_ok=(\d+) parse_unsupported=(\d+) "
        r"parse_malformed=(\d+) flow_hit=(\d+) route_hit=(\d+) "
        r"lookup_miss=(\d+) action_drop=(\d+) action_forward=(\d+) "
        r"action_rewrite=(\d+) tx_accepted=(\d+) tx_unsent=(\d+) drop=(\d+)",
        log_text,
    )
    if aggregate is None:
        raise RuntimeError("缺少 aggregate stats")
    workers = re.findall(
        r"worker: id=(\d+) lcore=(\d+) rxq=(\d+) txq=(\d+) rx=(\d+) "
        r"flow_hit=(\d+) route_hit=(\d+) drop=(\d+) tx=(\d+)", log_text)
    return list(map(int, aggregate.groups())), [tuple(map(int, row)) for row in workers]


def classify(offered, tx_packets, tx_unsent):
    if offered == 0:
        return "no offered traffic"
    ratio = tx_packets / offered
    if ratio >= 0.98 and tx_unsent == 0:
        return "generator/load path limited; router forwarded nearly all offered packets"
    if tx_unsent > 0:
        return "router TX short return observed in software TAP path"
    return "software TAP/kernel path loss before router counters; generator and kernel are coupled"


def run_case(root, env, workers, packet_size, flow_count, warmup, measure):
    cpus = sorted(os.sched_getaffinity(0))[:workers]
    if len(cpus) != workers:
        raise RuntimeError("可用 CPU 数不足以运行 %d workers" % workers)
    lcore_map = ",".join("%d@%d" % pair for pair in enumerate(cpus))
    suffix = uuid.uuid4().hex[:7]
    interfaces = ("brx" + suffix, "btx" + suffix)
    owned = {}
    process = None
    injector = None
    capture = None
    capture_stop = threading.Event()
    capture_count = [0]
    error = None

    with tempfile.TemporaryDirectory(prefix="dfr-goal008009-benchmark-") as temp:
        temp_path = Path(temp)
        rules_path = temp_path / "rules.json"
        log_path = temp_path / "router.log"
        rules_path.write_text(json.dumps(make_rules(flow_count)) + "\n")
        frames = [build_frame(packet_size, flow_id) for flow_id in range(flow_count)]
        command = [
            str(root / "bin/flow-router"), "--workers", str(workers),
            "--rules-file", str(rules_path), "--rx-device", "net_tap_rx",
            "--tx-device", "net_tap_tx", "--", "--lcores=" + lcore_map,
            "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "384",
            "--vdev=net_tap_rx,iface=" + interfaces[0],
            "--vdev=net_tap_tx,iface=" + interfaces[1],
        ]

        def remember_interfaces():
            for name in interfaces:
                path = Path("/sys/class/net") / name / "ifindex"
                try:
                    owned.setdefault(name, int(path.read_text()))
                except OSError:
                    pass

        def capture_loop():
            while not capture_stop.is_set():
                try:
                    packet, address = capture.recvfrom(65535)
                except socket.timeout:
                    continue
                if address[2] != socket.PACKET_OUTGOING and MARKER in packet:
                    capture_count[0] += 1

        with log_path.open("w+") as log:
            try:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while "dataplane ready\n" not in log_path.read_text():
                    remember_interfaces()
                    if process.poll() is not None:
                        raise RuntimeError("benchmark router ready 前退出:\n" +
                                           log_path.read_text())
                    if time.monotonic() >= deadline:
                        raise TimeoutError("benchmark router ready 超时")
                    time.sleep(0.03)
                remember_interfaces()
                for name in interfaces:
                    subprocess.run(["ip", "link", "set", "dev", name, "up"],
                                   check=True, timeout=3)
                injector = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
                injector.bind((interfaces[0], 0))
                capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                        socket.htons(3))
                capture.bind((interfaces[1], 0))
                capture.settimeout(0.1)
                capture_thread = threading.Thread(target=capture_loop, daemon=True)
                capture_thread.start()

                cpu_before = process_cpu_seconds(process.pid)
                offered = 0
                active_start = time.monotonic()
                active_end = active_start + warmup + measure
                flow_id = 0
                while time.monotonic() < active_end:
                    frame = frames[flow_id]
                    if injector.send(frame) == len(frame):
                        offered += 1
                    flow_id += 1
                    if flow_id == flow_count:
                        flow_id = 0
                elapsed = time.monotonic() - active_start
                cpu_after = process_cpu_seconds(process.pid)
                time.sleep(0.2)
                capture_stop.set()
                capture_thread.join(timeout=2)
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=8)
                if process.returncode != 0:
                    raise RuntimeError("benchmark router 非正常退出")
                log_text = log_path.read_text()
                aggregate, worker_rows = parse_stats(log_text)
                if len(worker_rows) != workers:
                    raise RuntimeError("benchmark worker stats 数量错误")
                rx_packets = aggregate[0]
                tx_packets = aggregate[10]
                drop_packets = aggregate[12]
                distribution = [row[5] for row in worker_rows]
                if sum(distribution) != aggregate[4]:
                    raise RuntimeError("benchmark worker flow_hit 聚合不守恒")
                if "pool_in_use=0 pool_freed=true" not in log_text or \
                        "EAL cleanup succeeded" not in log_text:
                    raise RuntimeError("benchmark cleanup evidence 不完整")
                cpu_seconds = cpu_after - cpu_before
                result = dict(env)
                result.update({
                    "workers": workers, "lcore_map": lcore_map,
                    "rx_queues": workers, "tx_queues": workers, "burst_size": 32,
                    "packet_size": packet_size, "flow_count": flow_count,
                    "warmup_seconds": "%.3f" % warmup,
                    "measure_seconds": "%.3f" % measure,
                    "duration": "%.6f" % elapsed,
                    "offered_packets": offered, "offered_pps": "%.3f" % (offered / elapsed),
                    "rx_packets": rx_packets, "tx_packets": tx_packets,
                    "captured_packets": capture_count[0], "drop_packets": drop_packets,
                    "rx_mpps": "%.6f" % (rx_packets / elapsed / 1e6),
                    "tx_mpps": "%.6f" % (tx_packets / elapsed / 1e6),
                    "gbps": "%.6f" % (tx_packets * packet_size * 8 / elapsed / 1e9),
                    "process_cpu_seconds": "%.6f" % cpu_seconds,
                    "cpu_utilization": "%.3f" % (cpu_seconds / elapsed * 100),
                    "worker_distribution": ";".join(map(str, distribution)),
                    "observed_bottleneck": classify(offered, tx_packets, aggregate[11]),
                })
            except Exception as exc:
                error = exc
                result = None
            finally:
                capture_stop.set()
                for sock in (capture, injector):
                    if sock is not None:
                        sock.close()
                remember_interfaces()
                if process is not None and process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                        error = error or RuntimeError("benchmark process cleanup 超时")
                for name, index in owned.items():
                    path = Path("/sys/class/net") / name
                    if path.exists():
                        if int((path / "ifindex").read_text()) != index:
                            error = error or RuntimeError("benchmark TAP ownership 已变化")
                            continue
                        subprocess.run(["ip", "link", "delete", "dev", name],
                                       check=False, timeout=3)
                        error = error or RuntimeError("benchmark TAP 未随 PMD close 消失")
                for name in interfaces:
                    if (Path("/sys/class/net") / name).exists():
                        error = error or RuntimeError("benchmark 后残留 TAP: " + name)
        if error is not None:
            raise error
        return result


def write_results(rows, csv_path, markdown_path, warmup, measure):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    lines = [
        "# Goal 008-009 Software Benchmark 结果", "",
        "本结果是 **software TAP / kernel / raw-socket end-to-end benchmark**。",
        "它包含 Linux kernel、TAP PMD、Python raw-socket generator/capture 与调度开销，",
        "不证明 physical NIC DMA、hardware RSS/RETA、NUMA NIC locality 或 line-rate。", "",
        "每个 case 使用 %.1fs warmup + %.1fs measurement；由于运行期统计只在 workers "
        "停止并 join 后聚合，表中 duration 与 counters 覆盖二者的连续总区间。" %
        (warmup, measure), "",
        "| workers | bytes | flows | offered pps | RX Mpps | TX Mpps | Gbps | CPU % | worker flow hits |", 
        "|---:|---:|---:|---:|---:|---:|---:|---:|:---|",
    ]
    for row in rows:
        lines.append("| {workers} | {packet_size} | {flow_count} | {offered_pps} | "
                     "{rx_mpps} | {tx_mpps} | {gbps} | {cpu_utilization} | "
                     "{worker_distribution} |".format(**row))
    lines.extend(["", "## 环境", "",
                  "- git commit: `%s`（worktree_dirty=%s）" %
                  (rows[0]["git_commit"], rows[0]["worktree_dirty"]),
                  "- DPDK: `%s`" % rows[0]["dpdk_version"],
                  "- kernel: `%s`" % rows[0]["kernel"],
                  "- CPU: `%s`" % rows[0]["cpu_model"],
                  "- allowed CPUs: `%s`" % rows[0]["allowed_cpu_list"],
                  "- NUMA: `%s`" % rows[0]["numa_topology"],
                  "- PMD/load path: `%s`" % rows[0]["pmd"], "",
                  "## 观察", ""])
    for workers in (1, 2, 4):
        subset = [row for row in rows if row["workers"] == workers]
        lines.append("- %d worker: TX Mpps 范围 %s～%s。" %
                     (workers, min(row["tx_mpps"] for row in subset),
                      max(row["tx_mpps"] for row in subset)))
    lines.extend([
        "- 本次结果由 Python generator 与 capture 同机运行；worker 增加不保证吞吐上升。",
        "- observed_bottleneck 的逐 case 判断保存在 CSV；任何下降都按 TAP/kernel、",
        "  generator/capture、scheduler、shared mempool 与 shared lookup cache 的组合开销解释。",
        "- explicit TAP rte_flow RSS：skipped；本实验环境没有 clang，optional eBPF/toolchain 路径未启用。",
        "- perf：未作为验收依赖；本次没有采集硬件 perf counters。",
    ])
    markdown_path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minimum", action="store_true",
                        help="正式最低矩阵：1/2/4 workers × 64/1500B × 1/1024 flows")
    parser.add_argument("--quick", action="store_true", help="开发冒烟：0.2s warmup + 1s measure")
    parser.add_argument("--csv", default="results/goal008009-software-benchmark.csv")
    parser.add_argument("--markdown", default="results/goal008009-software-benchmark.md")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if not (root / "bin/flow-router").is_file():
        raise RuntimeError("请先执行 make build")
    sizes = (64, 1500) if args.minimum else (64, 256, 1500)
    warmup, measure = (0.2, 1.0) if args.quick else (1.0, 5.0)
    env = environment(root)
    rows = []
    for workers in (1, 2, 4):
        for packet_size in sizes:
            for flow_count in (1, 1024):
                print("running workers=%d size=%d flows=%d" %
                      (workers, packet_size, flow_count), flush=True)
                rows.append(run_case(root, env, workers, packet_size, flow_count,
                                     warmup, measure))
    write_results(rows, root / args.csv, root / args.markdown, warmup, measure)
    print("PASS: %d software benchmark cases; no process/TAP/temp leak" % len(rows))


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        traceback.print_exc()
        raise SystemExit("FAIL: " + (str(exc) or type(exc).__name__))
