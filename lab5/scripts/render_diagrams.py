#!/usr/bin/env python3

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "diagrams"

CHAIN3 = [
    ("A", "B", "HELLO id=A"),
    ("B", "A", "HELLO id=B"),
    ("B", "C", "HELLO id=B"),
    ("C", "B", "HELLO id=C"),
    ("A", "B", "CHAT msgid=1 dest=C ttl=3 text=ping"),
    ("B", "C", "CHAT msgid=1 dest=C ttl=2"),
    ("C", None, "DELIVER msgid=1"),
    ("A", "B", "CHAT msgid=2 dest=Z ttl=3"),
    ("B", "C", "CHAT msgid=2 dest=Z ttl=2"),
    ("C", None, "NO_ROUTE msgid=2"),
    ("A", "B", "CHAT msgid=3 dest=Z ttl=1"),
    ("B", "C", "CHAT msgid=3 dest=Z ttl=0"),
    ("C", None, "DROP_TTL msgid=3"),
    ("A", "B", "закрытие TCP"),
    ("B", "C", "закрытие TCP"),
]

CHAIN4 = [
    ("A", "B", "HELLO id=A"),
    ("B", "A", "HELLO id=B"),
    ("B", "C", "HELLO id=B"),
    ("C", "B", "HELLO id=C"),
    ("C", "D", "HELLO id=C"),
    ("D", "C", "HELLO id=D"),
    ("A", "B", "CHAT msgid=10 dest=D ttl=5"),
    ("B", "C", "CHAT msgid=10 dest=D ttl=4"),
    ("C", "D", "CHAT msgid=10 dest=D ttl=3"),
    ("D", None, "DELIVER msgid=10"),
    ("A", "B", "CHAT msgid=11 dest=D ttl=1"),
    ("B", "C", "CHAT msgid=11 dest=D ttl=0"),
    ("C", None, "DROP_TTL msgid=11"),
]


def draw_sequence(participants, events, title, name):
    step = 0.72
    top = 0.5
    bottom = -step * (len(events) + 1)
    positions = {participant: index * 2.6
                 for index, participant in enumerate(participants)}

    figure, axes = plt.subplots(figsize=(14, max(6, 0.72 * len(events) + 2)))
    axes.set_xlim(-1.2, (len(participants) - 1) * 2.6 + 3.4)
    axes.set_ylim(bottom - 0.5, top + 0.9)
    axes.axis("off")
    axes.set_title(title, fontsize=14, pad=16)

    for participant in participants:
        x = positions[participant]
        axes.plot([x, x], [top, bottom], linestyle="--", linewidth=1.1,
                  color="#8ea9c9")
        axes.add_patch(
            FancyBboxPatch(
                (x - 0.55, top - 0.15),
                1.1,
                0.42,
                boxstyle="round,pad=0.08",
                facecolor="#dce6f2",
                edgecolor="#2f5597",
                linewidth=1.1,
            )
        )
        axes.text(x, top + 0.06, participant, ha="center", va="center",
                  fontsize=11, fontweight="bold")

    for index, (source, target, label) in enumerate(events):
        y = -step * (index + 1)
        x_source = positions[source]
        if target is None:
            text = axes.text(
                x_source + 0.35,
                y,
                label,
                ha="left",
                va="center",
                fontsize=8,
                bbox=dict(boxstyle="round,pad=0.3", facecolor="#fff2cc",
                          edgecolor="#bf9000", linewidth=0.9),
            )
            text.set_clip_on(False)
            continue

        x_target = positions[target]
        axes.annotate(
            "",
            xy=(x_target, y),
            xytext=(x_source, y),
            arrowprops=dict(arrowstyle="->", linewidth=1.3,
                            color="#2f5597", shrinkA=2, shrinkB=2),
        )
        axes.text(
            (x_source + x_target) / 2,
            y + 0.13,
            label,
            ha="center",
            va="bottom",
            fontsize=8,
            bbox=dict(boxstyle="round,pad=0.15", facecolor="white",
                      edgecolor="none", alpha=0.85),
        )

    figure.tight_layout()
    figure.savefig(OUTPUT / f"{name}.png", dpi=170, bbox_inches="tight")
    figure.savefig(OUTPUT / f"{name}.pdf", bbox_inches="tight")
    plt.close(figure)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    plt.rcParams["font.family"] = "DejaVu Sans"
    draw_sequence(
        ["A", "B", "C"],
        CHAIN3,
        "Вариант 6: последовательность обмена в цепочке A-B-C",
        "sequence_chain3",
    )
    draw_sequence(
        ["A", "B", "C", "D"],
        CHAIN4,
        "Вариант 6: последовательность обмена в цепочке A-B-C-D",
        "sequence_chain4",
    )
    print(f"diagrams written to {OUTPUT}")


if __name__ == "__main__":
    main()
