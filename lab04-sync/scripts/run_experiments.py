#!/usr/bin/env python3

import argparse
import csv
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "lab04_sync"
DATA = ROOT / "data"
RAW = DATA / "raw"

SEED = 3716
STUDENT_ID = 28

CONFIGS = [
    {
        "name": "standard_fifo",
        "series": "standard",
        "mode": "fifo",
        "philosophers": 5,
        "cycles": 100,
        "think_min": 1,
        "think_max": 5,
        "eat_min": 5,
        "eat_max": 15,
    },
    {
        "name": "individual_fifo",
        "series": "individual",
        "mode": "fifo",
        "philosophers": 5,
        "cycles": 240,
        "think_min": 1,
        "think_max": 2,
        "eat_min": 20,
        "eat_max": 40,
    },
    {
        "name": "individual_scan",
        "series": "individual",
        "mode": "scan",
        "philosophers": 5,
        "cycles": 240,
        "think_min": 1,
        "think_max": 2,
        "eat_min": 20,
        "eat_max": 40,
    },
    {
        "name": "load_n3_fifo",
        "series": "load",
        "mode": "fifo",
        "philosophers": 3,
        "cycles": 240,
        "think_min": 1,
        "think_max": 2,
        "eat_min": 20,
        "eat_max": 40,
    },
    {
        "name": "load_n5_fifo",
        "series": "load",
        "mode": "fifo",
        "philosophers": 5,
        "cycles": 240,
        "think_min": 1,
        "think_max": 2,
        "eat_min": 20,
        "eat_max": 40,
    },
    {
        "name": "load_n7_fifo",
        "series": "load",
        "mode": "fifo",
        "philosophers": 7,
        "cycles": 240,
        "think_min": 1,
        "think_max": 2,
        "eat_min": 20,
        "eat_max": 40,
    },
]

RESULT_FIELDS = [
    "config",
    "series",
    "run",
    "mode",
    "philosophers",
    "cycles",
    "seed",
    "think_min_ms",
    "think_max_ms",
    "eat_min_ms",
    "eat_max_ms",
    "elapsed_ms",
    "throughput_ops_s",
    "completed_cycles",
    "expected_cycles",
    "timeouts",
    "overtakes",
    "avg_wait_ms",
    "max_wait_ms",
    "max_queue",
    "grants",
    "invariant_violations",
    "status",
    "exit_code",
]


def parse_summary(text):
    summary = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Z_]+)=(.*)$", line)
        if match:
            summary[match.group(1)] = match.group(2)
    return summary


def run_one(binary, config, run_number, runs_dir):
    wait_csv = runs_dir / f"{config['name']}_run{run_number}_waits.csv"
    command = [
        str(binary),
        "--philosophers", str(config["philosophers"]),
        "--cycles", str(config["cycles"]),
        "--student-id", str(STUDENT_ID),
        "--seed", str(SEED),
        "--mode", config["mode"],
        "--think-min-ms", str(config["think_min"]),
        "--think-max-ms", str(config["think_max"]),
        "--eat-min-ms", str(config["eat_min"]),
        "--eat-max-ms", str(config["eat_max"]),
        "--timeout-ms", "30000",
        "--max-duration-ms", "180000",
        "--wait-csv", str(wait_csv),
        "--quiet",
    ]
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=210,
    )
    (runs_dir / f"{config['name']}_run{run_number}.txt").write_text(
        completed.stdout, encoding="utf-8"
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{config['name']} run {run_number} failed with exit code "
            f"{completed.returncode}\n{completed.stdout}\n{completed.stderr}"
        )

    summary = parse_summary(completed.stdout)
    completed_cycles, expected_cycles = summary["COMPLETED_CYCLES"].split("/", 1)
    return completed, summary, completed_cycles, expected_cycles, wait_csv


def write_environment(path):
    lines = [
        "environment",
        f"timestamp={datetime.now().isoformat(timespec='seconds')}",
        f"platform={platform.platform()}",
        f"machine={platform.machine()}",
        f"processor={platform.processor()}",
        f"python={platform.python_version()}",
    ]
    try:
        uname = subprocess.run(
            ["uname", "-a"], stdout=subprocess.PIPE, text=True, timeout=5
        ).stdout.strip()
        lines.append(f"uname={uname}")
    except Exception:
        pass
    try:
        cpu = subprocess.run(
            ["lscpu"], stdout=subprocess.PIPE, text=True, timeout=5
        ).stdout
        for line in cpu.splitlines():
            if line.startswith(("Model name:", "CPU(s):", "Thread(s) per core:",
                                "Core(s) per socket:")):
                lines.append(line.replace(":", "=", 1))
    except Exception:
        pass
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output", type=Path, default=DATA)
    args = parser.parse_args()

    if not args.binary.exists():
        print("Binary not found:", args.binary, file=sys.stderr)
        print("Build first: cmake --build lab04-sync/build -j", file=sys.stderr)
        return 1
    if args.runs < 1:
        print("--runs must be positive", file=sys.stderr)
        return 1

    output = args.output
    raw = output / "raw"
    runs_dir = raw / "runs"
    if runs_dir.exists():
        shutil.rmtree(runs_dir)
    runs_dir.mkdir(parents=True, exist_ok=True)

    results = []
    waits = []

    for config in CONFIGS:
        for run_number in range(1, args.runs + 1):
            print(
                f"[{config['name']}] run {run_number}/{args.runs}",
                flush=True,
            )
            completed, summary, completed_cycles, expected_cycles, wait_csv = run_one(
                args.binary, config, run_number, runs_dir
            )
            results.append({
                "config": config["name"],
                "series": config["series"],
                "run": run_number,
                "mode": config["mode"],
                "philosophers": config["philosophers"],
                "cycles": config["cycles"],
                "seed": SEED,
                "think_min_ms": config["think_min"],
                "think_max_ms": config["think_max"],
                "eat_min_ms": config["eat_min"],
                "eat_max_ms": config["eat_max"],
                "elapsed_ms": summary["ELAPSED_MS"],
                "throughput_ops_s": summary["THROUGHPUT_OPS_S"],
                "completed_cycles": completed_cycles,
                "expected_cycles": expected_cycles,
                "timeouts": summary["TIMEOUTS"],
                "overtakes": summary["OVERTAKES"],
                "avg_wait_ms": summary["AVG_WAIT_MS"],
                "max_wait_ms": summary["MAX_WAIT_MS"],
                "max_queue": summary["MAX_QUEUE"],
                "grants": summary["GRANTS"],
                "invariant_violations": summary["INVARIANT_VIOLATIONS"],
                "status": summary["STATUS"],
                "exit_code": completed.returncode,
            })

            with wait_csv.open(newline="", encoding="utf-8") as stream:
                for row in csv.DictReader(stream):
                    waits.append({
                        "config": config["name"],
                        "series": config["series"],
                        "run": run_number,
                        "mode": config["mode"],
                        "philosopher": row["philosopher"],
                        "wait_ms": row["wait_ms"],
                        "result": row["result"],
                    })

    with (output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=RESULT_FIELDS)
        writer.writeheader()
        writer.writerows(results)

    with (output / "waits.csv").open("w", newline="", encoding="utf-8") as stream:
        fieldnames = [
            "config", "series", "run", "mode", "philosopher", "wait_ms", "result"
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(waits)

    write_environment(output / "environment.txt")
    print(f"Saved {len(results)} runs to {output / 'results.csv'}")
    print(f"Saved {len(waits)} wait records to {output / 'waits.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
