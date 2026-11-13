#!/usr/bin/env python3

import csv
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "lab06_regwatch"
RAW_DIR = ROOT / "data" / "raw"
LOG_DIR = RAW_DIR / "logs"
WORK_DIR = RAW_DIR / "work"

STUDENT_ID = "028"
SEED = 3716
RUNS = 3
PREFIX = "HKEY_CURRENT_USER/Software/OSiSP/Lab06/Student_028"

DETAIL_PATTERN = re.compile(r"(\w+)=([^\s]+)")
LOG_PATTERN = re.compile(
    r"^\[(?P<timestamp>[^\]]+)\] (?P<role>\S+) event=(?P<event>\S+)"
    r"(?: (?P<details>.*))?$"
)

CONFIGS = [
    {
        "name": "values_1",
        "initial_values": 1,
        "operation": "modify",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
    {
        "name": "values_10",
        "initial_values": 10,
        "operation": "modify",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
    {
        "name": "values_100",
        "initial_values": 100,
        "operation": "modify",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
    {
        "name": "values_500",
        "initial_values": 500,
        "operation": "modify",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
    {
        "name": "change_1",
        "initial_values": 100,
        "operation": "modify",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
    {
        "name": "change_5",
        "initial_values": 100,
        "operation": "modify",
        "requested_changes": 5,
        "debounce_ms": 300,
    },
    {
        "name": "change_10",
        "initial_values": 100,
        "operation": "modify",
        "requested_changes": 10,
        "debounce_ms": 300,
    },
    {
        "name": "delete_1",
        "initial_values": 100,
        "operation": "delete",
        "requested_changes": 1,
        "debounce_ms": 60,
    },
]


def run_command(args, expect=0):
    process = subprocess.run(
        [str(BINARY)] + args,
        text=True,
        capture_output=True,
    )
    if process.returncode != expect:
        raise RuntimeError(
            f"command {' '.join(args)} returned {process.returncode}\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}"
        )
    return process


def read_log(path):
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = LOG_PATTERN.match(line)
        if match:
            entry = match.groupdict()
            entry["details"] = entry["details"] or ""
            entry["fields"] = dict(
                DETAIL_PATTERN.findall(entry["details"])
            )
            entries.append(entry)
    return entries


def start_monitor(work, key, log_path, ready_path, debounce_ms):
    command = [
        str(BINARY), "monitor",
        "--root", str(work),
        "--student-id", STUDENT_ID,
        "--key", key,
        "--timeout-ms", "6000",
        "--max-events", "1",
        "--debounce-ms", str(debounce_ms),
        "--ready-file", str(ready_path),
        "--log-file", str(log_path),
        "--quiet",
    ]
    return subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def wait_ready(process, ready_path, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ready_path.exists():
            return
        if process.poll() is not None:
            break
        time.sleep(0.005)
    stdout, stderr = process.communicate(timeout=2.0)
    raise RuntimeError(
        f"monitor did not become ready\nstdout:\n{stdout}\nstderr:\n{stderr}"
    )


def wait_monitor(process, timeout=10.0):
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.communicate(timeout=2.0)
        raise RuntimeError("monitor did not stop in time")
    if process.returncode != 0:
        raise RuntimeError(
            f"monitor returned {process.returncode}: stdout={stdout} "
            f"stderr={stderr}"
        )


def run_once(config, run_index):
    name = config["name"]
    work = WORK_DIR / f"{name}_run{run_index}"
    log_path = LOG_DIR / f"{name}_run{run_index}.log"
    ready_path = work / "ready"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.exists():
        log_path.unlink()

    run_id = f"{config['initial_values']}_{run_index}"
    key = f"{PREFIX}/Run_{run_id}"
    run_command([
        "init", "--root", str(work), "--student-id", STUDENT_ID,
        "--run-id", run_id, "--count", str(config["initial_values"]),
        "--seed", str(SEED), "--quiet",
    ])

    process = start_monitor(
        work, key, log_path, ready_path, config["debounce_ms"]
    )
    wait_ready(process, ready_path)

    start_ns = time.time_ns()
    if config["operation"] == "modify":
        for index in range(config["requested_changes"]):
            run_command([
                "set", "--root", str(work), "--student-id", STUDENT_ID,
                "--key", key, "--name", f"Item_{index:03d}",
                "--type", "REG_SZ", "--value", f"changed-{run_index}-{index}",
                "--quiet",
            ])
    elif config["operation"] == "delete":
        run_command([
            "delete", "--root", str(work), "--student-id", STUDENT_ID,
            "--key", key, "--name", "Item_000", "--quiet",
        ])
    else:
        raise RuntimeError(f"unknown operation {config['operation']}")
    wait_monitor(process)

    entries = read_log(log_path)
    notifies = [entry for entry in entries if entry["event"] == "REG_NOTIFY"]
    diffs = [entry for entry in entries if entry["event"] == "REG_DIFF"]
    stop = next(
        (entry for entry in entries if entry["event"] == "MONITOR_STOP"), None
    )
    status = stop["fields"].get("status", "MISSING") if stop else "MISSING"

    latency_ms = ""
    if notifies:
        ts_us = int(notifies[0]["fields"].get("ts_us", "0"))
        latency_ms = (ts_us - start_ns / 1000.0) / 1000.0

    fields = diffs[0]["fields"] if diffs else {}
    added = int(fields.get("added", 0))
    removed = int(fields.get("removed", 0))
    changed = int(fields.get("changed", 0))
    keys_added = int(fields.get("keys_added", 0))
    keys_removed = int(fields.get("keys_removed", 0))
    detected_total = added + removed + changed + keys_added + keys_removed
    read_us = int(fields.get("read_us", 0))

    shutil.rmtree(work, ignore_errors=True)

    return {
        "config": name,
        "run": run_index,
        "initial_values": config["initial_values"],
        "operation": config["operation"],
        "requested_changes": config["requested_changes"],
        "debounce_ms": config["debounce_ms"],
        "latency_ms": latency_ms,
        "read_ms": read_us / 1000.0,
        "notifications": len(notifies),
        "added": added,
        "removed": removed,
        "changed": changed,
        "keys_added": keys_added,
        "keys_removed": keys_removed,
        "detected_total": detected_total,
        "status": status,
    }


def write_summary(rows):
    groups = {}
    for row in rows:
        groups.setdefault(row["config"], []).append(row)

    summary = []
    for config in CONFIGS:
        group = groups.get(config["name"], [])
        latencies = [row["latency_ms"] for row in group
                     if row["latency_ms"] != ""]
        reads = [row["read_ms"] for row in group]
        errors = sum(1 for row in group if row["status"] != "OK")
        summary.append({
            "config": config["name"],
            "initial_values": config["initial_values"],
            "operation": config["operation"],
            "requested_changes": config["requested_changes"],
            "debounce_ms": config["debounce_ms"],
            "runs": len(group),
            "mean_latency_ms": f"{statistics.mean(latencies):.3f}"
            if latencies else "",
            "min_latency_ms": f"{min(latencies):.3f}" if latencies else "",
            "max_latency_ms": f"{max(latencies):.3f}" if latencies else "",
            "mean_read_ms": f"{statistics.mean(reads):.3f}" if reads else "",
            "mean_detected_total": f"{statistics.mean([row['detected_total'] for row in group]):.2f}"
            if group else "",
            "mean_notifications": f"{statistics.mean([row['notifications'] for row in group]):.2f}"
            if group else "",
            "errors": errors,
        })
    return summary


def main():
    if not BINARY.exists():
        print(f"binary not found: {BINARY}", file=sys.stderr)
        return 1

    RAW_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    rows = []
    for config in CONFIGS:
        for run_index in range(1, RUNS + 1):
            print(f"{config['name']} run {run_index}...", flush=True)
            row = run_once(config, run_index)
            print(
                f"  latency={row['latency_ms']} ms, "
                f"detected={row['detected_total']}, "
                f"notifications={row['notifications']}, status={row['status']}",
                flush=True,
            )
            rows.append(row)

    raw_path = RAW_DIR / "experiment_raw.csv"
    with raw_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    summary = write_summary(rows)
    summary_path = RAW_DIR / "summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0].keys()))
        writer.writeheader()
        writer.writerows(summary)

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    errors = sum(1 for row in rows if row["status"] != "OK")
    print(f"raw data: {raw_path}")
    print(f"summary: {summary_path}")
    print(f"runs: {len(rows)}, errors: {errors}")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
