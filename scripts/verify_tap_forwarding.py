#!/usr/bin/env python3
"""双 TAP 软件端到端验证；只操作本进程创建且 ifindex 匹配的接口。"""
import argparse
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

MARKER = b"dpdk-flow-router-goal002"
ETHERTYPE = 0x88B5


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
        # 名称启动前不存在，启动后记录 ifindex；清理时拒绝删除同名替代接口。
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

    # 临时日志在成功与失败后都删除；异常时先打印必要诊断，不提交原始日志。
    with tempfile.TemporaryDirectory(prefix="dfr-goal002-") as temp:
        log_path = Path(temp) / "router.log"
        with log_path.open("w+") as log:
            try:
                command = [str(binary), "--rx-device", "missing_tap" if args.bad_device else "net_tap_rx",
                           "--tx-device", "net_tap_tx", "--", "--lcores=0@" + str(cpu),
                           "--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "64",
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
                    # 仅将本次 TAP 设为 UP，不配置 IP、route、firewall 或其他接口。
                    subprocess.run(["ip", "link", "set", "dev", name, "up"],
                                   check=True, timeout=remaining())
                capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
                capture.bind((tx_iface, 0))
                injector = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
                injector.bind((rx_iface, 0))
                # 确定性的 60 字节 Ethernet frame（不含 FCS），无 parser/IP 依赖。
                frame = bytes.fromhex("020000000002020000000001") + struct.pack("!H", ETHERTYPE)
                frame += MARKER.ljust(46, b"\x00")
                if not args.skip_injection:
                    injector.settimeout(remaining())
                    if injector.send(frame) != len(frame):
                        raise RuntimeError("raw socket 未完整注入 frame")
                while True:
                    capture.settimeout(remaining())
                    packet, address = capture.recvfrom(65535)
                    if address[2] == socket.PACKET_OUTGOING:
                        continue
                    if len(packet) >= 14 and packet[12:14] == struct.pack("!H", ETHERTYPE):
                        if packet != frame:
                            raise AssertionError("TX EtherType 命中，但完整 frame/marker 不一致")
                        print("exact marker captured: EtherType=0x88b5 marker=" + MARKER.decode()
                              + " frame_bytes=" + str(len(packet)), flush=True)
                        break
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=remaining())
                if process.returncode != 0:
                    raise RuntimeError("router 非正常退出: " + str(process.returncode))
                log_text = log_path.read_text()
                match = re.search(r"stats: rx=(\d+) tx_accepted=(\d+) tx_unsent=(\d+) drop=(\d+)", log_text)
                if not match:
                    raise AssertionError("缺少最终 stats")
                rx, tx, unsent, drop = map(int, match.groups())
                if not (rx >= 1 and tx >= 1 and rx == tx + unsent and drop == unsent):
                    raise AssertionError("stats 不满足 mbuf ownership 守恒")
                if "teardown: ports_closed=2 pool_in_use=0 pool_freed=true" not in log_text:
                    raise AssertionError("缺少 port close / pool 归还与释放证据")
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
                # 非 persist TAP 正常随 PMD close 消失。若仍存在，仅删除已记录的同一接口。
                for name, index in owned.items():
                    path = Path("/sys/class/net") / name
                    if path.exists():
                        try:
                            if int((path / "ifindex").read_text()) != index or not (path / "tun_flags").exists():
                                raise RuntimeError("接口 ownership 变化，拒绝删除: " + name)
                            subprocess.run(["ip", "link", "delete", "dev", name], check=True, timeout=3)
                            # 成功测试不允许依靠兜底删除掩盖 PMD cleanup 问题。
                            error = error or RuntimeError("TAP 未随 PMD close 消失，已兜底删除: " + name)
                        except Exception as exc:
                            error = error or exc
                for name in interfaces:
                    if (Path("/sys/class/net") / name).exists():
                        error = error or RuntimeError("测试后仍存在 TAP: " + name)
        print(log_text, end="")
    if error is not None:
        raise RuntimeError(str(error) or type(error).__name__)
    print("PASS: exact TAP forwarding, graceful cleanup, no test interfaces/process/temp files remain")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        raise SystemExit("FAIL: " + (str(exc) or type(exc).__name__))
