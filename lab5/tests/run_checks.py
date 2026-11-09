#!/usr/bin/env python3

import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("LAB05_BIN", ROOT / "build" / "peer_chat"))


class CheckFailure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailure(message)


def free_port():
    for _ in range(200):
        port = random.randint(23000, 29999)
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            sock.close()
    raise RuntimeError("cannot allocate a free port")


def frame(payload):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return struct.pack("!I", len(payload)) + payload


def recv_exact(sock, length):
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("socket closed while reading")
        data += chunk
    return data


def recv_frame(sock):
    length = struct.unpack("!I", recv_exact(sock, 4))[0]
    return recv_exact(sock, length)


def read_text(path):
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return ""


def start_peer(workdir, node_id, port, peers=(), run_ms=3000, extra=()):
    log_path = workdir / f"{node_id}.log"
    command = [
        str(BINARY),
        "--id", node_id,
        "--port", str(port),
        "--run-ms", str(run_ms),
        "--connect-wait-ms", "150",
        "--log-file", str(log_path),
        "--quiet",
    ]
    for peer_port in peers:
        command += ["--peer", f"127.0.0.1:{peer_port}"]
    command += list(extra)
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return process, log_path


def wait_all(processes, timeout=12.0):
    deadline = time.monotonic() + timeout
    for process in processes:
        remaining = max(0.1, deadline - time.monotonic())
        try:
            process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
    for process in processes:
        if process.returncode not in (0, -15):
            stderr = process.stderr.read() if process.stderr else ""
            raise CheckFailure(
                f"peer exited with code {process.returncode}: {stderr}"
            )


def run_chain(workdir, ids, edges, sender_extra, receiver_run_ms=3000):
    ports = {node_id: free_port() for node_id in ids}
    processes = []
    logs = {}
    for node_id in reversed(ids[1:]):
        peers = [ports[target] for source, target in edges if source == node_id]
        process, log_path = start_peer(
            workdir, node_id, ports[node_id], peers,
            run_ms=receiver_run_ms,
        )
        processes.append(process)
        logs[node_id] = log_path
        time.sleep(0.35)

    extra = [
        "--peer", f"127.0.0.1:{ports[ids[1]]}",
        "--run-ms", "1200",
    ] + list(sender_extra)
    sender, sender_log = start_peer(
        workdir, ids[0], ports[ids[0]], (), run_ms=1200, extra=extra
    )
    processes.append(sender)
    logs[ids[0]] = sender_log

    wait_all(processes)
    return {node_id: read_text(path) for node_id, path in logs.items()}


def test_delivery_through_intermediate(workdir):
    logs = run_chain(
        workdir,
        ["A", "B", "C"],
        [("A", "B"), ("B", "C")],
        ["--messages", "1", "--dest", "C", "--text", "ping", "--ttl", "3"],
    )
    require("event=DELIVER" in logs["C"], "C did not deliver the message")
    require("text=ping #1" in logs["C"], "delivered text is wrong")
    require("event=FORWARD" in logs["B"], "B did not forward the message")
    require("delivered=1" in logs["C"], "C delivered counter is wrong")
    require("forwarded=1" in logs["B"], "B forwarded counter is wrong")


def test_unknown_destination(workdir):
    logs = run_chain(
        workdir,
        ["A", "B", "C"],
        [("A", "B"), ("B", "C")],
        ["--messages", "1", "--dest", "Z", "--text", "lost", "--ttl", "3"],
    )
    require("event=DELIVER" not in logs["C"], "unknown message was delivered")
    require("event=NO_ROUTE" in logs["C"], "C did not report NO_ROUTE")
    require("dropped_no_route=1" in logs["C"], "NO_ROUTE counter is wrong")


def test_ttl_termination(workdir):
    logs = run_chain(
        workdir,
        ["A", "B", "C"],
        [("A", "B"), ("B", "C")],
        ["--messages", "1", "--dest", "Z", "--text", "lost", "--ttl", "1"],
    )
    require("event=DROP_TTL" in logs["C"], "C did not drop the TTL-expired message")
    require("dropped_ttl=1" in logs["C"], "TTL counter is wrong")
    require("event=DELIVER" not in logs["C"], "TTL-expired message was delivered")


def test_duplicates_are_suppressed(workdir):
    port_c = free_port()
    port_b = free_port()
    c_process, c_log = start_peer(workdir, "C", port_c, run_ms=2600)
    time.sleep(0.35)
    b_process, b_log = start_peer(
        workdir, "B", port_b, [port_c], run_ms=2600
    )
    processes = [b_process, c_process]
    try:
        time.sleep(0.8)
        payload = (
            "type=CHAT\nver=1\nid=X\nmsgid=dup-1\ndest=C\nttl=3\ntext=once\n"
        )
        with socket.create_connection(("127.0.0.1", port_b), timeout=3) as sock:
            sock.sendall(frame(payload))
            time.sleep(0.1)
            sock.sendall(frame(payload))
            time.sleep(0.8)
    finally:
        wait_all(processes)

    b_text = read_text(b_log)
    c_text = read_text(c_log)
    require("dropped_dup=1" in b_text, "B did not suppress the duplicate")
    require("forwarded=1" in b_text, "B forwarded the duplicate twice")
    require("delivered=1" in c_text, "C received duplicate deliveries")


def test_protocol_error_and_partial_receive(workdir):
    port = free_port()
    process, log_path = start_peer(workdir, "P", port, run_ms=2800)
    try:
        time.sleep(0.4)
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            hello_frame = recv_frame(sock)
            require(b"type=HELLO" in hello_frame, "peer did not send HELLO")

            bad = "type=CHAT\nver=1\nid=X\nmsgid=bad\nttl=3\ntext=broken\n"
            sock.sendall(frame(bad))
            error_frame = recv_frame(sock)
            require(b"type=ERROR" in error_frame, "peer did not answer with ERROR")

            good = (
                "type=CHAT\nver=1\nid=X\nmsgid=good-1\ndest=P\n"
                "ttl=1\ntext=slow\n"
            ).encode("utf-8")
            encoded = frame(good)
            for byte in encoded:
                sock.sendall(bytes([byte]))
                time.sleep(0.002)
            time.sleep(0.5)
    finally:
        wait_all([process])

    text = read_text(log_path)
    require("event=PROTOCOL_ERROR" in text, "protocol error was not logged")
    require("event=DELIVER" in text, "partially received message was not delivered")


def test_unavailable_server(workdir):
    port = free_port()
    missing = free_port()
    process, log_path = start_peer(
        workdir, "S", port, [missing], run_ms=900
    )
    wait_all([process], timeout=6.0)
    text = read_text(log_path)
    require("event=CONNECT_FAIL" in text, "connection failure was not logged")


def main():
    if not BINARY.exists():
        print(f"binary not found: {BINARY}", file=sys.stderr)
        return 2

    random.seed(3716)
    workdir = Path(tempfile.mkdtemp(prefix="lab05-checks-"))
    tests = [
        ("delivery through intermediate", test_delivery_through_intermediate),
        ("unknown destination", test_unknown_destination),
        ("TTL termination", test_ttl_termination),
        ("duplicate suppression", test_duplicates_are_suppressed),
        ("protocol error and partial receive",
         test_protocol_error_and_partial_receive),
        ("unavailable server", test_unavailable_server),
    ]

    failed = 0
    try:
        for name, function in tests:
            test_dir = workdir / name.replace(" ", "_")
            test_dir.mkdir(parents=True, exist_ok=True)
            try:
                function(test_dir)
                print(f"PASS: {name}")
            except Exception as error:  # noqa: BLE001
                failed += 1
                print(f"FAIL: {name}: {error}", file=sys.stderr)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
