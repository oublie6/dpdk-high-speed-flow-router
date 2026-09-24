#!/usr/bin/env python3
"""Goal 008-009 TAP multi-queue affinity/spread 与双 worker 动态规则回归。"""
import json
from collections import Counter
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import uuid


SRC_IP = "192.0.2.10"
DST_IP = "198.51.100.20"
SRC_PORT = 12000
DST_PORT = 22000


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def udp_frame(src_port, dst_port, payload, packet_id):
    ethernet = bytes.fromhex("0200000000020200000000010800")
    src = socket.inet_aton(SRC_IP)
    dst = socket.inet_aton(DST_IP)
    udp_len = 8 + len(payload)
    ip_len = 20 + udp_len
    ip0 = struct.pack("!BBHHHBBH4s4s", 0x45, 0, ip_len, packet_id, 0,
                      64, socket.IPPROTO_UDP, 0, src, dst)
    ip_header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, ip_len, packet_id, 0,
                            64, socket.IPPROTO_UDP, checksum(ip0), src, dst)
    udp0 = struct.pack("!HHHH", src_port, dst_port, udp_len, 0)
    pseudo = src + dst + struct.pack("!BBH", 0, socket.IPPROTO_UDP, udp_len)
    udp_sum = checksum(pseudo + udp0 + payload)
    if udp_sum == 0:
        udp_sum = 0xffff
    udp_header = struct.pack("!HHHH", src_port, dst_port, udp_len, udp_sum)
    return ethernet + ip_header + udp_header + payload


def rules_for_flows(flow_count):
    flows = []
    for index in range(flow_count):
        flows.append({
            "src_ipv4": SRC_IP,
            "dst_ipv4": DST_IP,
            "src_port": SRC_PORT + index,
            "dst_port": DST_PORT + index,
            "protocol": "udp",
            "action": {"type": "forward"},
        })
    return {"flows": flows, "routes": []}


def parse_worker_stats(log_text):
    pattern = re.compile(
        r"worker: id=(\d+) lcore=(\d+) rxq=(\d+) txq=(\d+) rx=(\d+) "
        r"flow_hit=(\d+) route_hit=(\d+) drop=(\d+) tx=(\d+) "
        r"tx_unsent=(\d+) rx_bursts=(\d+) empty=(\d+) tx_bursts=(\d+)"
    )
    return [tuple(map(int, match)) for match in pattern.findall(log_text)]


def run_case(binary, workers, flow_count, rounds, marker_prefix):
    cpus = sorted(os.sched_getaffinity(0))[:workers]
    if len(cpus) != workers:
        raise RuntimeError("可用 CPU 数不足以启动 %d workers" % workers)
    lcores = ",".join("%d@%d" % pair for pair in enumerate(cpus))
    suffix = uuid.uuid4().hex[:8]
    interfaces = ("mqrx" + suffix, "mqtx" + suffix)
    owned = {}
    process = None
    capture = None
    injector = None
    error = None
    log_text = ""

    with tempfile.TemporaryDirectory(prefix="dfr-goal008009-functional-") as temp:
        temp_path = Path(temp)
        rules_path = temp_path / "rules.json"
        log_path = temp_path / "router.log"
        rules_path.write_text(json.dumps(rules_for_flows(flow_count)) + "\n")
        command = [
            str(binary), "--workers", str(workers), "--rules-file", str(rules_path),
            "--rx-device", "net_tap_rx", "--tx-device", "net_tap_tx", "--",
            "--lcores=" + lcores, "--no-huge", "--no-pci", "--no-shconf",
            "--no-telemetry", "-m", "256",
            "--vdev=net_tap_rx,iface=" + interfaces[0],
            "--vdev=net_tap_tx,iface=" + interfaces[1],
        ]
        deadline = time.monotonic() + 30

        def remember_interfaces():
            for name in interfaces:
                path = Path("/sys/class/net") / name / "ifindex"
                try:
                    owned.setdefault(name, int(path.read_text()))
                except FileNotFoundError:
                    pass

        with log_path.open("w+") as log:
            try:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                while "dataplane ready\n" not in log_path.read_text():
                    remember_interfaces()
                    if process.poll() is not None:
                        raise RuntimeError("router ready 前退出: %d" % process.returncode)
                    if time.monotonic() >= deadline:
                        raise TimeoutError("等待 router ready 超时")
                    time.sleep(0.03)
                remember_interfaces()
                for name in interfaces:
                    subprocess.run(["ip", "link", "set", "dev", name, "up"],
                                   check=True, timeout=3)

                capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                        socket.htons(3))
                capture.bind((interfaces[1], 0))
                injector = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
                injector.bind((interfaces[0], 0))

                expected = []
                for round_id in range(rounds):
                    for flow_id in range(flow_count):
                        marker = marker_prefix + struct.pack("!HH", round_id, flow_id)
                        frame = udp_frame(SRC_PORT + flow_id, DST_PORT + flow_id,
                                          marker, 0x5000 + round_id * flow_count + flow_id)
                        if injector.send(frame) != len(frame):
                            raise RuntimeError("raw socket 未完整发送 frame")
                        expected.append(frame)

                received = []
                while len(received) < len(expected):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("等待 TX frame 超时")
                    capture.settimeout(min(0.5, remaining))
                    try:
                        packet, address = capture.recvfrom(65535)
                    except socket.timeout:
                        continue
                    if address[2] == socket.PACKET_OUTGOING or marker_prefix not in packet:
                        continue
                    received.append(packet)
                frames_match = received == expected if flow_count == 1 else \
                    Counter(received) == Counter(expected)
                if not frames_match:
                    mismatch = next((index for index, pair in enumerate(zip(received, expected))
                                     if pair[0] != pair[1]), -1)
                    if mismatch >= 0:
                        raise AssertionError(
                            "single-flow sequence 或转发 frame 不一致: index=%d got=%s want=%s" %
                            (mismatch, received[mismatch].hex(), expected[mismatch].hex()))
                    raise AssertionError("single-flow frame 数量不一致")

                process.send_signal(signal.SIGTERM)
                process.wait(timeout=5)
                if process.returncode != 0:
                    raise RuntimeError("router 非正常退出: %d" % process.returncode)
                log_text = log_path.read_text()
                worker_stats = parse_worker_stats(log_text)
                if len(worker_stats) != workers:
                    raise AssertionError("worker stats 数量不匹配")
                for worker_id, lcore_id, rxq, txq, *_ in worker_stats:
                    if worker_id != rxq or worker_id != txq:
                        raise AssertionError("queue single-owner mapping 不成立")
                    mapping = ("worker mapping: id=%d lcore=%d RXQ%d -> worker%d -> TXQ%d" %
                               (worker_id, lcore_id, rxq, worker_id, txq))
                    if mapping not in log_text:
                        raise AssertionError("缺少显式 worker mapping")
                distribution = [row[5] for row in worker_stats]
                if sum(distribution) != len(expected):
                    raise AssertionError("flow_hit aggregate 与 deterministic packet 数不等")
                if flow_count == 1 and sum(value != 0 for value in distribution) != 1:
                    raise AssertionError("同一 5-tuple 出现在多个 worker")
                if flow_count > 1 and workers == 2 and any(value == 0 for value in distribution):
                    raise AssertionError("32 flows 未分布到两个 workers")
                if "pool_in_use=0 pool_freed=true" not in log_text or \
                        "snapshot_freed=true qsbr_freed=true" not in log_text or \
                        "EAL cleanup succeeded" not in log_text:
                    raise AssertionError("cleanup evidence 不完整")
            except Exception as exc:
                error = exc
            finally:
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
                        error = error or RuntimeError("router graceful stop 超时")
                for name, index in owned.items():
                    path = Path("/sys/class/net") / name
                    if path.exists():
                        try:
                            if int((path / "ifindex").read_text()) != index:
                                raise RuntimeError("TAP ownership 已变化，拒绝删除")
                            subprocess.run(["ip", "link", "delete", "dev", name],
                                           check=True, timeout=3)
                            error = error or RuntimeError("TAP 未随 PMD close 消失")
                        except Exception as exc:
                            error = error or exc
                for name in interfaces:
                    if (Path("/sys/class/net") / name).exists():
                        error = error or RuntimeError("测试后仍残留 TAP: " + name)
        if error is not None:
            raise error
        return distribution


def main():
    root = Path(__file__).resolve().parent.parent
    binary = root / "bin/flow-router"
    if not binary.is_file():
        raise RuntimeError("请先执行 make build")

    affinity = run_case(binary, 2, 1, 32, b"goal008-affinity-")
    spread2 = run_case(binary, 2, 32, 2, b"goal008-spread2-")
    spread4 = run_case(binary, 4, 32, 2, b"goal008-spread4-")
    subprocess.run([str(root / "scripts/verify_tap_forwarding.sh"), "--workers", "2"],
                   check=True, timeout=60)

    print("single-flow affinity distribution:", affinity)
    print("32-flow / 2-worker distribution:", spread2)
    print("32-flow / 4-worker distribution:", spread4)
    print("explicit TAP rte_flow RSS: skipped (optional path; clang unavailable in this lab)")
    print("PASS: software TAP/kernel flow affinity, multi-flow spread, fixed queue ownership, "
          "2-worker dynamic rules, graceful cleanup, no process/TAP/temp leak")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        raise SystemExit("FAIL: " + (str(exc) or type(exc).__name__))
