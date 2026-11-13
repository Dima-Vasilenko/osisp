#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("LAB06_BIN", ROOT / "build" / "lab06_regwatch"))
STUDENT = "028"
PREFIX = "HKEY_CURRENT_USER/Software/OSiSP/Lab06/Student_028"

LOG_PATTERN = re.compile(
    r"^\[(?P<timestamp>[^\]]+)\] (?P<role>\S+) event=(?P<event>\S+)"
    r"(?: (?P<details>.*))?$"
)


class CheckFailure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailure(message)


def run(args, expect=0):
    process = subprocess.run(
        [str(BINARY)] + args,
        text=True,
        capture_output=True,
    )
    if process.returncode != expect:
        raise CheckFailure(
            f"command {' '.join(args)} returned {process.returncode}, "
            f"expected {expect}\nstdout:\n{process.stdout}\n"
            f"stderr:\n{process.stderr}"
        )
    return process


def read_log(path):
    if not path.exists():
        return []
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = LOG_PATTERN.match(line)
        if match:
            entries.append(match.groupdict())
    return entries


def start_monitor(root, key, log_path, ready_path, extra=()):
    command = [
        str(BINARY), "monitor",
        "--root", str(root),
        "--student-id", STUDENT,
        "--key", key,
        "--log-file", str(log_path),
        "--ready-file", str(ready_path),
        "--quiet",
    ] + list(extra)
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
        time.sleep(0.01)
    stdout, stderr = process.communicate(timeout=2.0)
    raise CheckFailure(
        f"monitor did not become ready\nstdout:\n{stdout}\nstderr:\n{stderr}"
    )


def wait_monitor(process, timeout=6.0):
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        stdout, stderr = process.communicate(timeout=2.0)
        raise CheckFailure("monitor did not stop in time")
    require(process.returncode == 0,
            f"monitor returned {process.returncode}: {stderr}")
    return stdout, stderr


def list_values(root, key):
    process = run([
        "list", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--quiet",
    ])
    values = {}
    for line in process.stdout.splitlines():
        if not line.startswith("VALUE\t"):
            continue
        parts = line.split("\t", 4)
        values[parts[2]] = {
            "key": parts[1],
            "name": parts[2],
            "type": parts[3],
            "value": parts[4],
        }
    return values


def test_init_list(root):
    run([
        "init", "--root", str(root), "--student-id", STUDENT,
        "--run-id", "1", "--count", "12", "--seed", "3716", "--quiet",
    ])
    key = f"{PREFIX}/Run_1"
    values = list_values(root, key)
    require(len(values) == 12, "list contains a wrong value count")
    types = {item["type"] for item in values.values()}
    require("REG_SZ" in types and "REG_DWORD" in types,
            "generated values do not cover expected types")
    require("Item_000" in values, "generated Item_000 is missing")


def test_set_and_delete(root):
    key = f"{PREFIX}/Run_2"
    run([
        "init", "--root", str(root), "--student-id", STUDENT,
        "--run-id", "2", "--count", "4", "--seed", "3716", "--quiet",
    ])

    run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "Alpha", "--type", "REG_DWORD",
        "--value", "123456", "--quiet",
    ])
    values = list_values(root, key)
    require(values["Alpha"]["type"] == "REG_DWORD", "REG_DWORD type lost")
    require(values["Alpha"]["value"] == "123456", "REG_DWORD value lost")

    run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "Alpha", "--type", "REG_SZ",
        "--value", "updated", "--quiet",
    ])
    values = list_values(root, key)
    require(values["Alpha"]["type"] == "REG_SZ", "type was not updated")
    require(values["Alpha"]["value"] == "updated", "value was not updated")

    run([
        "delete", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "Alpha", "--quiet",
    ])
    values = list_values(root, key)
    require("Alpha" not in values, "deleted value is still listed")


def test_errors(root):
    key = f"{PREFIX}/Run_3"
    run([
        "init", "--root", str(root), "--student-id", STUDENT,
        "--run-id", "3", "--count", "2", "--seed", "3716", "--quiet",
    ])

    outside = run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", "HKEY_CURRENT_USER/Software/Other",
        "--name", "X", "--value", "Y",
    ], expect=1)
    require("write denied" in outside.stderr, "outside write was not denied")

    bad_type = run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "X", "--type", "REG_NOPE",
        "--value", "1",
    ], expect=1)
    require("unknown value type" in bad_type.stderr,
            "unknown type was not reported")

    bad_dword = run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "X", "--type", "REG_DWORD",
        "--value", "abc",
    ], expect=1)
    require("non-negative decimal" in bad_dword.stderr,
            "invalid REG_DWORD was not reported")

    missing = run([
        "monitor", "--root", str(root), "--student-id", STUDENT,
        "--key", f"{PREFIX}/Run_999", "--timeout-ms", "200", "--quiet",
    ], expect=1)
    require("does not exist" in missing.stderr,
            "missing key error was not reported")


def prepare_run(root, run_id, count):
    key = f"{PREFIX}/Run_{run_id}"
    run([
        "init", "--root", str(root), "--student-id", STUDENT,
        "--run-id", str(run_id), "--count", str(count), "--seed", "3716",
        "--quiet",
    ])
    return key


def test_monitor_modify(root):
    key = prepare_run(root, 4, 20)
    log_path = root / "modify.log"
    ready_path = root / "modify.ready"
    process = start_monitor(
        root, key, log_path, ready_path,
        ["--timeout-ms", "4000", "--max-events", "1", "--debounce-ms", "80"],
    )
    wait_ready(process, ready_path)
    run([
        "set", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "Item_000", "--value", "after", "--quiet",
    ])
    wait_monitor(process)

    entries = read_log(log_path)
    events = [entry["event"] for entry in entries]
    require("REG_NOTIFY" in events, "REG_NOTIFY was not logged")
    require("REG_MODIFY" in events, "REG_MODIFY was not logged")
    modify = next(entry for entry in entries if entry["event"] == "REG_MODIFY")
    require("name=Item_000" in modify["details"], "wrong modified name")
    require("old=text-3716-0" in modify["details"], "old value is missing")
    require("new=after" in modify["details"], "new value is missing")
    require("status=OK" in next(
        entry for entry in entries if entry["event"] == "MONITOR_STOP"
    )["details"], "monitor did not stop normally")


def test_monitor_multi(root):
    key = prepare_run(root, 5, 20)
    log_path = root / "multi.log"
    ready_path = root / "multi.ready"
    process = start_monitor(
        root, key, log_path, ready_path,
        ["--timeout-ms", "5000", "--max-events", "1", "--debounce-ms", "300"],
    )
    wait_ready(process, ready_path)
    for index in range(5):
        run([
            "set", "--root", str(root), "--student-id", STUDENT,
            "--key", key, "--name", f"Item_{index:03d}",
            "--value", f"batch-{index}", "--quiet",
        ])
    wait_monitor(process)

    entries = read_log(log_path)
    diff = next(entry for entry in entries if entry["event"] == "REG_DIFF")
    require("changed=5" in diff["details"],
            f"expected five changed values: {diff['details']}")
    modified = [entry for entry in entries if entry["event"] == "REG_MODIFY"]
    require(len(modified) == 5, "not all modifications were described")


def test_monitor_delete(root):
    key = prepare_run(root, 6, 10)
    log_path = root / "delete.log"
    ready_path = root / "delete.ready"
    process = start_monitor(
        root, key, log_path, ready_path,
        ["--timeout-ms", "4000", "--max-events", "1", "--debounce-ms", "80"],
    )
    wait_ready(process, ready_path)
    run([
        "delete", "--root", str(root), "--student-id", STUDENT,
        "--key", key, "--name", "Item_003", "--quiet",
    ])
    wait_monitor(process)

    entries = read_log(log_path)
    diff = next(entry for entry in entries if entry["event"] == "REG_DIFF")
    require("removed=1" in diff["details"], "deleted value was not counted")
    deleted = next(entry for entry in entries if entry["event"] == "REG_DEL")
    require("name=Item_003" in deleted["details"], "wrong deleted value")


def main():
    if not BINARY.exists():
        print(f"binary not found: {BINARY}", file=sys.stderr)
        return 1

    base = Path(tempfile.mkdtemp(prefix="lab06_checks_"))
    tests = [
        ("init and list", test_init_list, "root1"),
        ("set and delete", test_set_and_delete, "root2"),
        ("error handling", test_errors, "root3"),
        ("monitor modify", test_monitor_modify, "root4"),
        ("monitor multi", test_monitor_multi, "root5"),
        ("monitor delete", test_monitor_delete, "root6"),
    ]
    try:
        for name, function, directory in tests:
            root = base / directory
            root.mkdir(parents=True)
            try:
                function(root)
            except CheckFailure as error:
                print(f"FAIL: {name}: {error}", file=sys.stderr)
                return 1
            print(f"ok: {name}")
    finally:
        shutil.rmtree(base, ignore_errors=True)

    print("all lab06 checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
