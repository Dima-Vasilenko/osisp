#!/usr/bin/env python3

import argparse
import csv
import random
import re
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "peer_chat"
RAW_DIR = ROOT / "data" / "raw"

LOG_PATTERN = re.compile(
    r"^\[(?P<timestamp>[^\]]+)\] ROLE=PEER id=(?P<id>\S+) "
    r"event=(?P<event>\S+)(?: (?P<details>.*))?$"
)
TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S.%f"

CONFIGURATIONS = {
    "chain3": {
        "ids": ["A", "B", "C"],
        "edges": [("A", "B"), ("B", "C")],
        "destination": "C",
    },
    "chain4": {
        "ids": ["A", "B", "C", "D"],
        "edges": [("A", "B"), ("B", "C"), ("C", "D")],
        "destination": "D",
    },
}


def free_port():
    for _ in range(300):
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


def start_peer(node_id, port, peers, run_ms, log_path, extra=()):
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
    return subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def wait_all(processes, timeout):
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
            raise RuntimeError(
                f"peer exited with code {process.returncode}: {stderr}"
            )


def parse_log(path):
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = LOG_PATTERN.match(line)
        if not match:
            continue
        timestamp = datetime.strptime(match.group("timestamp"),
                                      TIMESTAMP_FORMAT)
        entries.append(
            {
                "timestamp": timestamp,
                "id": match.group("id"),
                "event": match.group("event"),
                "details": match.group("details") or "",
            }
        )
    return entries


def counters(entries):
    for entry in entries:
        if entry["event"] == "SUMMARY":
            return {
                key: int(value)
                for key, value in re.findall(r"(\w+)=(\d+)",
                                             entry["details"])
            }
    return {}


def run_once(config_name, run_index, work_root):
    configuration = CONFIGURATIONS[config_name]
    ids = configuration["ids"]
    edges = configuration["edges"]
    destination = configuration["destination"]

    run_dir = work_root / f"{config_name}_run{run_index}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)

    ports = {node_id: free_port() for node_id in ids}
    processes = []
    for node_id in reversed(ids[1:]):
        peers = [ports[target] for source, target in edges
                 if source == node_id]
        processes.append(
            start_peer(node_id, ports[node_id], peers, 4000,
                       run_dir / f"{node_id}.log")
        )
        time.sleep(0.35)

    extra = [
        "--peer", f"127.0.0.1:{ports[ids[1]]}",
        "--messages", "20",
        "--dest", destination,
        "--text", "payload",
        "--ttl", "5",
        "--send-interval-ms", "50",
        "--run-ms", "1500",
    ]
    sender = start_peer(
        ids[0], ports[ids[0]], [], 1500, run_dir / f"{ids[0]}.log", extra
    )
    processes.append(sender)
    wait_all(processes, timeout=20.0)

    logs = {node_id: parse_log(run_dir / f"{node_id}.log")
            for node_id in ids}
    sender_entries = logs[ids[0]]
    destination_entries = logs[destination]

    send_times = {}
    for entry in sender_entries:
        if entry["event"] == "SEND_CHAT":
            match = re.search(r"msgid=(\S+)", entry["details"])
            if match:
                send_times[match.group(1)] = entry["timestamp"]

    latencies = []
    latency_rows = []
    matched_send_times = []
    matched_delivery_times = []
    for entry in destination_entries:
        if entry["event"] != "DELIVER":
            continue
        match = re.search(r"msgid=(\S+)", entry["details"])
        if not match:
            continue
        message_id = match.group(1)
        if message_id not in send_times:
            continue
        latency_match = re.search(r"latency_us=(\d+)", entry["details"])
        if latency_match:
            latency_ms = int(latency_match.group(1)) / 1000.0
        else:
            latency_ms = (
                entry["timestamp"] - send_times[message_id]
            ).total_seconds() * 1000.0
        latencies.append(latency_ms)
        matched_send_times.append(send_times[message_id])
        matched_delivery_times.append(entry["timestamp"])
        latency_rows.append(
            {
                "config": config_name,
                "run": run_index,
                "msgid": message_id,
                "latency_ms": f"{latency_ms:.3f}",
            }
        )

    sender_counters = counters(sender_entries)
    destination_counters = counters(destination_entries)
    forwarded = 0
    dropped_ttl = 0
    dropped_dup = 0
    dropped_no_route = 0
    protocol_errors = 0
    for node_id in ids[1:]:
        node_counters = counters(logs[node_id])
        forwarded += node_counters.get("forwarded", 0)
        dropped_ttl += node_counters.get("dropped_ttl", 0)
        dropped_dup += node_counters.get("dropped_dup", 0)
        dropped_no_route += node_counters.get("dropped_no_route", 0)
        protocol_errors += node_counters.get("protocol_errors", 0)

    delivery_span_ms = 0.0
    if matched_send_times and matched_delivery_times:
        delivery_span_ms = (
            max(matched_delivery_times) - min(matched_send_times)
        ).total_seconds() * 1000.0
    throughput = 0.0
    if delivery_span_ms > 0:
        throughput = len(latencies) * 1000.0 / delivery_span_ms

    result = {
        "config": config_name,
        "run": run_index,
        "nodes": len(ids),
        "messages_sent": sender_counters.get("sent_chat", 0),
        "messages_delivered": destination_counters.get("delivered", 0),
        "forwarded": forwarded,
        "dropped_ttl": dropped_ttl,
        "dropped_dup": dropped_dup,
        "dropped_no_route": dropped_no_route,
        "protocol_errors": protocol_errors,
        "avg_latency_ms": (f"{sum(latencies) / len(latencies):.3f}"
                           if latencies else "0.000"),
        "min_latency_ms": (f"{min(latencies):.3f}"
                           if latencies else "0.000"),
        "max_latency_ms": (f"{max(latencies):.3f}"
                           if latencies else "0.000"),
        "delivery_span_ms": f"{delivery_span_ms:.3f}",
        "throughput_msg_s": f"{throughput:.3f}",
        "bytes_sent_sender": sender_counters.get("bytes_sent", 0),
        "bytes_received_dest": destination_counters.get(
            "bytes_received", 0
        ),
    }
    return result, latency_rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--only", choices=sorted(CONFIGURATIONS),
                        action="append")
    parser.add_argument("--output", type=Path, default=RAW_DIR)
    args = parser.parse_args()

    if not BINARY.exists():
        print(f"binary not found: {BINARY}; build the project first",
              file=sys.stderr)
        return 1

    random.seed(3716)
    args.output.mkdir(parents=True, exist_ok=True)

    configurations = args.only or sorted(CONFIGURATIONS)
    results = []
    latencies = []
    for config_name in configurations:
        for run_index in range(1, args.runs + 1):
            print(f"running {config_name} run {run_index}...")
            result, run_latencies = run_once(
                config_name, run_index, args.output
            )
            results.append(result)
            latencies.extend(run_latencies)

    results_path = args.output / "experiment_raw.csv"
    with results_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0].keys()))
        writer.writeheader()
        writer.writerows(results)

    latency_path = args.output / "latency_raw.csv"
    with latency_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["config", "run", "msgid", "latency_ms"]
        )
        writer.writeheader()
        writer.writerows(latencies)

    print(f"wrote {results_path}")
    print(f"wrote {latency_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
