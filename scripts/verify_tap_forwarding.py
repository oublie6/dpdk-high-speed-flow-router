#!/usr/bin/env python3
"""Goal 004-005 双 TAP 软件端到端验证；只操作本进程创建的接口。"""
import argparse
import json
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

ETHERTYPE = 0x0800
SRC_IP = "192.0.2.1"
FLOW_DROP_DST = "198.51.100.10"
FLOW_REWRITE_DST = "203.0.113.20"
REWRITTEN_DST = "192.0.2.99"
ROUTE_FORWARD_DST = "198.51.100.30"
LOOKUP_MISS_DST = "203.0.113.40"
SRC_PORT = 12345
FLOW_DROP_PORT = 20001
FLOW_REWRITE_PORT = 20002
REWRITTEN_PORT = 30002
ROUTE_FORWARD_PORT = 20003
LOOKUP_MISS_PORT = 20004

MARKER_FLOW_DROP = b"goal004-flow-drop"
MARKER_FLOW_REWRITE = b"goal004-flow-rewrite"
MARKER_ROUTE_FORWARD = b"goal004-route-forward"
MARKER_LOOKUP_MISS = b"goal004-lookup-miss"
MARKER_MALFORMED = b"goal003-malformed"


def internet_checksum(data):
    """返回按网络字节序写入 header 的 RFC 1071 checksum 数值。"""
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def build_udp_frame(dst_ip, dst_port, marker, packet_id):
    ethernet = bytes.fromhex("020000000002020000000001") + struct.pack("!H", ETHERTYPE)
    src_bytes = socket.inet_aton(SRC_IP)
    dst_bytes = socket.inet_aton(dst_ip)
    udp_length = 8 + len(marker)
    ipv4_length = 20 + udp_length
    ipv4_without_checksum = struct.pack(
        "!BBHHHBBH4s4s", 0x45, 0, ipv4_length, packet_id, 0, 64,
        socket.IPPROTO_UDP, 0, src_bytes, dst_bytes,
    )
    ipv4_checksum = internet_checksum(ipv4_without_checksum)
    ipv4 = struct.pack(
        "!BBHHHBBH4s4s", 0x45, 0, ipv4_length, packet_id, 0, 64,
        socket.IPPROTO_UDP, ipv4_checksum, src_bytes, dst_bytes,
    )
    udp_without_checksum = struct.pack("!HHHH", SRC_PORT, dst_port, udp_length, 0)
    pseudo = src_bytes + dst_bytes + struct.pack("!BBH", 0, socket.IPPROTO_UDP, udp_length)
    udp_checksum = internet_checksum(pseudo + udp_without_checksum + marker)
    if udp_checksum == 0:
        udp_checksum = 0xFFFF
    udp = struct.pack("!HHHH", SRC_PORT, dst_port, udp_length, udp_checksum)
    return ethernet + ipv4 + udp + marker


def build_malformed_frame():
    """IHL=4 小于 IPv4 最小值；marker 用于断言该包绝不能出现在 TX。"""
    ethernet = bytes.fromhex("020000000002020000000001") + struct.pack("!H", ETHERTYPE)
    payload_length = 8 + len(MARKER_MALFORMED)
    ipv4 = struct.pack(
        "!BBHHHBBH4s4s", 0x44, 0, 20 + payload_length, 0x4005, 0, 64,
        socket.IPPROTO_UDP, 0, socket.inet_aton(SRC_IP),
        socket.inet_aton(LOOKUP_MISS_DST),
    )
    udp = struct.pack("!HHHH", SRC_PORT, 29999, payload_length, 0)
    return ethernet + ipv4 + udp + MARKER_MALFORMED


def verify_ipv4_udp_checksums(frame):
    ipv4 = bytearray(frame[14:34])
    stored_ipv4 = struct.unpack("!H", ipv4[10:12])[0]
    ipv4[10:12] = b"\x00\x00"
    if stored_ipv4 != internet_checksum(bytes(ipv4)):
        raise AssertionError("rewritten IPv4 checksum is invalid")
    udp = bytearray(frame[34:])
    stored_udp = struct.unpack("!H", udp[6:8])[0]
    udp[6:8] = b"\x00\x00"
    udp_length = struct.unpack("!H", udp[4:6])[0]
    pseudo = frame[26:30] + frame[30:34] + struct.pack(
        "!BBH", 0, socket.IPPROTO_UDP, udp_length,
    )
    expected_udp = internet_checksum(pseudo + bytes(udp[:udp_length]))
    if expected_udp == 0:
        expected_udp = 0xFFFF
    if stored_udp != expected_udp:
        raise AssertionError("rewritten UDP checksum is invalid")


def write_rules(path):
    rules = {
        "flows": [
            {
                "src_ipv4": SRC_IP, "dst_ipv4": FLOW_DROP_DST,
                "src_port": SRC_PORT, "dst_port": FLOW_DROP_PORT,
                "protocol": "udp", "action": {"type": "drop"},
            },
            {
                "src_ipv4": SRC_IP, "dst_ipv4": FLOW_REWRITE_DST,
                "src_port": SRC_PORT, "dst_port": FLOW_REWRITE_PORT,
                "protocol": "udp",
                "action": {"type": "rewrite", "dst_ipv4": REWRITTEN_DST,
                           "dst_port": REWRITTEN_PORT},
            },
        ],
        "routes": [
            {"prefix": "198.0.0.0/8", "action": {"type": "drop"}},
            {"prefix": "198.51.100.0/24", "action": {"type": "forward"}},
        ],
    }
    path.write_text(json.dumps(rules, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=30, help="主体测试的整体超时秒数")
    parser.add_argument("--skip-injection", action="store_true", help="负向验收：不注入，验证超时与清理")
    parser.add_argument("--bad-device", action="store_true", help="负向验收：setup 失败仍须清理 EAL/TAP")
    args = parser.parse_args()
    if not 0 < args.timeout <= 120:
        parser.error("timeout 必须在 (0,120] 秒内")
    root = Path(__file__).resolve().parent.parent
    binary = root / "bin/flow-router"
    if not binary.is_file():
        raise RuntimeError("请先执行 make build")
    suffix = uuid.uuid4().hex[:8]
    rx_iface, tx_iface = "dfrx" + suffix, "dftx" + suffix
    interfaces = (rx_iface, tx_iface)
    for name in interfaces:
        if (Path("/sys/class/net") / name).exists():
            raise RuntimeError("随机接口名已存在，拒绝复用: " + name)
    cpu = min(os.sched_getaffinity(0))
    deadline = time.monotonic() + args.timeout
    owned = {}
    process = None
    capture = injector = None
    error = None
    log_text = ""

    def remaining():
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("TAP 验证整体超时")
        return left

    def remember_interfaces():
        for name in interfaces:
            index_file = Path("/sys/class/net") / name / "ifindex"
            try:
                owned.setdefault(name, int(index_file.read_text()))
            except FileNotFoundError:
                pass

    def stop_process():
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
                raise RuntimeError("graceful stop 超时，已强制回收测试子进程")

    with tempfile.TemporaryDirectory(prefix="dfr-goal004005-") as temp:
        log_path = Path(temp) / "router.log"
        rules_path = Path(temp) / "rules.json"
        write_rules(rules_path)
        with log_path.open("w+") as log:
            try:
                command = [str(binary), "--rules-file", str(rules_path),
                           "--rx-device", "missing_tap" if args.bad_device else "net_tap_rx",
                           "--tx-device", "net_tap_tx", "--", "--lcores=0@" + str(cpu),
                           "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "256",
                           "--vdev=net_tap_rx,iface=" + rx_iface,
                           "--vdev=net_tap_tx,iface=" + tx_iface]
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                while True:
                    remaining()
                    remember_interfaces()
                    if "dataplane ready\n" in log_path.read_text():
                        break
                    if process.poll() is not None:
                        raise RuntimeError("router 在 ready 前退出，status=" + str(process.returncode))
                    time.sleep(min(0.05, remaining()))
                for name in interfaces:
                    if name not in owned:
                        raise RuntimeError("ready 后缺少 TAP: " + name)
                    subprocess.run(["ip", "link", "set", "dev", name, "up"],
                                   check=True, timeout=remaining())
                capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
                capture.bind((tx_iface, 0))
                injector = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
                injector.bind((rx_iface, 0))

                flow_drop = build_udp_frame(FLOW_DROP_DST, FLOW_DROP_PORT,
                                            MARKER_FLOW_DROP, 0x4001)
                flow_rewrite = build_udp_frame(FLOW_REWRITE_DST, FLOW_REWRITE_PORT,
                                               MARKER_FLOW_REWRITE, 0x4002)
                expected_rewrite = build_udp_frame(REWRITTEN_DST, REWRITTEN_PORT,
                                                   MARKER_FLOW_REWRITE, 0x4002)
                route_forward = build_udp_frame(ROUTE_FORWARD_DST, ROUTE_FORWARD_PORT,
                                                MARKER_ROUTE_FORWARD, 0x4003)
                lookup_miss = build_udp_frame(LOOKUP_MISS_DST, LOOKUP_MISS_PORT,
                                              MARKER_LOOKUP_MISS, 0x4004)
                malformed = build_malformed_frame()
                frames = [malformed, flow_drop, lookup_miss, flow_rewrite, route_forward]
                if not args.skip_injection:
                    injector.settimeout(remaining())
                    for frame in frames:
                        if injector.send(frame) != len(frame):
                            raise RuntimeError("raw socket 未完整注入测试 frame")

                captured = set()
                forbidden = (MARKER_FLOW_DROP, MARKER_LOOKUP_MISS, MARKER_MALFORMED)
                while captured != {"rewrite", "route"}:
                    capture.settimeout(remaining())
                    packet, address = capture.recvfrom(65535)
                    if address[2] == socket.PACKET_OUTGOING:
                        continue
                    if any(marker in packet for marker in forbidden):
                        raise AssertionError("DROP/malformed marker 被错误转发")
                    if MARKER_FLOW_REWRITE in packet:
                        if packet != expected_rewrite:
                            raise AssertionError("flow REWRITE frame 与预期不一致")
                        verify_ipv4_udp_checksums(packet)
                        captured.add("rewrite")
                    if MARKER_ROUTE_FORWARD in packet:
                        if packet != route_forward:
                            raise AssertionError("route FORWARD 未保持完整 frame")
                        captured.add("route")
                print("flow precedence DROP PASS; UDP REWRITE + checksum PASS; "
                      "route /24 FORWARD unchanged PASS; lookup miss DROP PASS", flush=True)

                process.send_signal(signal.SIGTERM)
                process.wait(timeout=remaining())
                if process.returncode != 0:
                    raise RuntimeError("router 非正常退出: " + str(process.returncode))
                log_text = log_path.read_text()
                match = re.search(
                    r"stats: rx=(\d+) parse_ok=(\d+) parse_unsupported=(\d+) "
                    r"parse_malformed=(\d+) flow_hit=(\d+) route_hit=(\d+) "
                    r"lookup_miss=(\d+) action_drop=(\d+) action_forward=(\d+) "
                    r"action_rewrite=(\d+) tx_accepted=(\d+) tx_unsent=(\d+) drop=(\d+)",
                    log_text,
                )
                if not match:
                    raise AssertionError("缺少最终 Goal004-005 stats")
                values = list(map(int, match.groups()))
                (rx, parse_ok, unsupported, malformed_count, flow_hit, route_hit,
                 lookup_miss_count, action_drop, action_forward, action_rewrite,
                 tx, unsent, drop) = values
                if malformed_count < 1 or flow_hit < 2 or route_hit < 1 or lookup_miss_count < 1:
                    raise AssertionError("deterministic parse/lookup case 未全部计数")
                if action_drop < 2 or action_forward < 1 or action_rewrite < 1:
                    raise AssertionError("deterministic action case 未全部计数")
                if rx != parse_ok + unsupported + malformed_count:
                    raise AssertionError("rx parser 分类守恒失败")
                if parse_ok != flow_hit + route_hit + lookup_miss_count:
                    raise AssertionError("parse_ok lookup 分类守恒失败")
                if parse_ok != action_drop + action_forward + action_rewrite:
                    raise AssertionError("parse_ok action 分类守恒失败")
                if action_forward + action_rewrite != tx + unsent:
                    raise AssertionError("action TX ownership 守恒失败")
                if drop != unsupported + malformed_count + action_drop + unsent:
                    raise AssertionError("drop ownership 守恒失败")
                teardown = ("teardown: ports_closed=2 pool_in_use=0 pool_freed=true "
                            "flow_table_freed=true route_table_freed=true "
                            "action_store_freed=true")
                if teardown not in log_text:
                    raise AssertionError("缺少 port/pool/lookup/action 完整释放证据")
                if "EAL cleanup succeeded" not in log_text:
                    raise AssertionError("缺少 EAL cleanup 成功证据")
            except Exception as exc:
                error = exc
            finally:
                for sock in (capture, injector):
                    if sock is not None:
                        sock.close()
                remember_interfaces()
                try:
                    stop_process()
                except Exception as exc:
                    error = error or exc
                log_text = log_path.read_text()
                for name, index in owned.items():
                    path = Path("/sys/class/net") / name
                    if path.exists():
                        try:
                            if int((path / "ifindex").read_text()) != index or not (path / "tun_flags").exists():
                                raise RuntimeError("接口 ownership 变化，拒绝删除: " + name)
                            subprocess.run(["ip", "link", "delete", "dev", name], check=True, timeout=3)
                            error = error or RuntimeError("TAP 未随 PMD close 消失，已兜底删除: " + name)
                        except Exception as exc:
                            error = error or exc
                for name in interfaces:
                    if (Path("/sys/class/net") / name).exists():
                        error = error or RuntimeError("测试后仍存在 TAP: " + name)
        print(log_text, end="")
    if error is not None:
        raise RuntimeError(str(error) or type(error).__name__)
    print("PASS: Goal004-005 exact flow/LPM/action/rewrite, stats conservation, "
          "graceful cleanup, no test interfaces/process/temp files remain")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        raise SystemExit("FAIL: " + (str(exc) or type(exc).__name__))
