#!/usr/bin/env python3

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIAGRAMS = ROOT / "diagrams"

SOURCES = ["architecture", "activity_monitor"]


def main():
    dot = shutil.which("dot")
    if dot is None:
        print("Graphviz 'dot' is not installed", file=sys.stderr)
        return 1

    for name in SOURCES:
        source = DIAGRAMS / f"{name}.dot"
        for extension in ("png", "pdf"):
            output = DIAGRAMS / f"{name}.{extension}"
            subprocess.run(
                [dot, f"-T{extension}", str(source), "-o", str(output)],
                check=True,
            )
            print(f"written {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
