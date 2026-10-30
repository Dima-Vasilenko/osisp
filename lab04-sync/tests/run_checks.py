#!/usr/bin/env python3

import csv
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = ROOT / "build" / "lab04_sync"
BINARY = Path(os.environ.get("LAB04_BIN", DEFAULT_BIN))


def run_model(arguments, timeout=30):
    command = [str(BINARY)] + arguments
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    summary = {}
    for line in completed.stdout.splitlines():
        match = re.match(r"^([A-Z_]+)=(.*)$", line)
        if match:
            summary[match.group(1)] = match.group(2)
    return completed, summary


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def test_fifo():
    completed, summary = run_model([
        "--philosophers", "5",
        "--cycles", "10",
        "--mode", "fifo",
        "--timeout-ms", "5000",
        "--max-duration-ms", "30000",
    ])
    check(completed.returncode == 0, "FIFO run failed: " + completed.stderr)
    check(summary.get("STATUS") == "OK", "FIFO status is not OK")
    check(summary.get("COMPLETED_CYCLES") == "50/50", "FIFO cycles mismatch")
    check(summary.get("INVARIANT_VIOLATIONS") == "0", "FIFO invariant violation")
    check(summary.get("OVERTAKES") == "0", "FIFO has overtakes")


def test_scan():
    completed, summary = run_model([
        "--philosophers", "5",
        "--cycles", "10",
        "--mode", "scan",
        "--timeout-ms", "5000",
        "--max-duration-ms", "30000",
    ])
    check(completed.returncode == 0, "SCAN run failed: " + completed.stderr)
    check(summary.get("STATUS") == "OK", "SCAN status is not OK")
    check(summary.get("COMPLETED_CYCLES") == "50/50", "SCAN cycles mismatch")
    check(summary.get("INVARIANT_VIOLATIONS") == "0", "SCAN invariant violation")


def test_timeout_and_csv():
    with tempfile.TemporaryDirectory() as directory:
        wait_csv = Path(directory) / "waits.csv"
        completed, summary = run_model([
            "--philosophers", "5",
            "--cycles", "10",
            "--mode", "fifo",
            "--think-min-ms", "0",
            "--think-max-ms", "0",
            "--eat-min-ms", "20",
            "--eat-max-ms", "20",
            "--timeout-ms", "1",
            "--max-duration-ms", "30000",
            "--wait-csv", str(wait_csv),
        ])
        check(completed.returncode == 0, "timeout run failed: " + completed.stderr)
        check(summary.get("STATUS") == "OK", "timeout run status is not OK")
        check(int(summary.get("TIMEOUTS", "0")) > 0, "timeout was not detected")
        check(summary.get("INVARIANT_VIOLATIONS") == "0",
              "timeout run invariant violation")
        check(wait_csv.exists(), "wait CSV was not created")
        with wait_csv.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        check(rows, "wait CSV has no records")
        check(rows[0].keys() == {"philosopher", "wait_ms", "result"},
              "unexpected wait CSV header")


def test_max_duration():
    completed, summary = run_model([
        "--philosophers", "5",
        "--cycles", "1000",
        "--mode", "scan",
        "--timeout-ms", "5000",
        "--max-duration-ms", "1",
    ])
    check(completed.returncode == 3, "max duration did not abort the run")
    check(summary.get("STATUS") == "ABORTED", "max duration status mismatch")
    check(summary.get("INVARIANT_VIOLATIONS") == "0",
          "max duration left invariant violations")


def test_event_log():
    with tempfile.TemporaryDirectory() as directory:
        log_file = Path(directory) / "events.log"
        completed, summary = run_model([
            "--philosophers", "3",
            "--cycles", "2",
            "--mode", "fifo",
            "--timeout-ms", "5000",
            "--max-duration-ms", "30000",
            "--log-file", str(log_file),
        ])
        check(completed.returncode == 0, "log run failed: " + completed.stderr)
        check(summary.get("STATUS") == "OK", "log run status is not OK")
        log_text = log_file.read_text(encoding="utf-8")
        for event in ("START", "WAITING", "ACQUIRED", "EATING",
                      "RELEASED", "FINISHED"):
            check(event in log_text, "event is missing from log: " + event)


def test_bad_arguments():
    completed, _ = run_model(["--philosophers", "1"])
    check(completed.returncode == 2, "invalid arguments returned wrong code")


def main():
    if not BINARY.exists():
        print("Binary not found:", BINARY, file=sys.stderr)
        print("Build the project first: cmake --build lab04-sync/build -j", file=sys.stderr)
        return 1

    tests = [
        ("fifo", test_fifo),
        ("scan", test_scan),
        ("timeout", test_timeout_and_csv),
        ("max-duration", test_max_duration),
        ("event-log", test_event_log),
        ("bad-arguments", test_bad_arguments),
    ]

    failures = 0
    for name, test in tests:
        try:
            test()
            print("[PASS]", name)
        except Exception as error:
            failures += 1
            print("[FAIL]", name, "-", error, file=sys.stderr)

    if failures:
        print("Failures:", failures, file=sys.stderr)
        return 1
    print("All checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
