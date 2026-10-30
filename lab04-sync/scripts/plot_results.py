#!/usr/bin/env python3

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA = ROOT / "data"
DEFAULT_OUTPUT = ROOT / "data" / "plots"


def read_rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def save_figure(figure, output, name):
    figure.savefig(output / f"{name}.png", dpi=160, bbox_inches="tight")
    figure.savefig(output / f"{name}.pdf", bbox_inches="tight")
    plt.close(figure)


def plot_throughput(results, output):
    rows = [
        row for row in results
        if row["series"] == "load" and row["mode"] == "fifo"
    ]
    if not rows:
        print("No load/FIFO rows for throughput plot")
        return

    grouped = defaultdict(list)
    for row in rows:
        grouped[int(row["philosophers"])].append(
            float(row["throughput_ops_s"])
        )

    philosophers = sorted(grouped)
    means = [float(np.mean(grouped[n])) for n in philosophers]
    lower = [float(np.mean(grouped[n]) - np.min(grouped[n])) for n in philosophers]
    upper = [float(np.max(grouped[n]) - np.mean(grouped[n])) for n in philosophers]

    figure, axes = plt.subplots(figsize=(7.5, 4.5))
    positions = np.arange(len(philosophers))
    axes.bar(positions, means, yerr=[lower, upper], capsize=6,
             color="#4472c4", edgecolor="#2f5597", width=0.55)
    axes.set_xticks(positions)
    axes.set_xticklabels(philosophers)
    axes.set_xlabel("Число философов")
    axes.set_ylabel("Пропускная способность, операций/с")
    axes.set_title("FIFO: пропускная способность при разном числе участников")
    axes.grid(axis="y", alpha=0.3)
    for position, value in zip(positions, means):
        axes.text(position, value, f"{value:.1f}", ha="center",
                  va="bottom", fontsize=9)
    figure.tight_layout()
    save_figure(figure, output, "throughput_vs_philosophers")


def collect_waits(waits, config):
    grouped = defaultdict(list)
    for row in waits:
        if row["config"] == config and row["result"] == "GRANTED":
            grouped[int(row["philosopher"])].append(float(row["wait_ms"]))
    return grouped


def plot_wait_distribution(waits, output):
    fifo = collect_waits(waits, "individual_fifo")
    scan = collect_waits(waits, "individual_scan")
    if not fifo or not scan:
        print("No individual FIFO/SCAN waits for distribution plot")
        return

    philosophers = sorted(set(fifo) | set(scan))
    figure, axes = plt.subplots(1, 2, figsize=(10, 4.8), sharey=True)
    for axis, data, title, color in (
        (axes[0], fifo, "FIFO", "#4472c4"),
        (axes[1], scan, "SCAN", "#ed7d31"),
    ):
        values = [data.get(philosopher, []) for philosopher in philosophers]
        box = axis.boxplot(values, patch_artist=True, showfliers=True,
                           medianprops={"color": "black"})
        for patch in box["boxes"]:
            patch.set_facecolor(color)
            patch.set_alpha(0.6)
        axis.set_xticks(range(1, len(philosophers) + 1))
        axis.set_xticklabels(philosophers)
        axis.set_xlabel("Философ")
        axis.set_title(title)
        axis.grid(axis="y", alpha=0.3)
    axes[0].set_ylabel("Время ожидания, мс")
    figure.suptitle("Распределение времени ожидания по философам")
    figure.tight_layout()
    save_figure(figure, output, "wait_distribution_fifo_vs_scan")


def plot_wait_summary(results, output):
    rows = [
        row for row in results
        if row["config"] in ("individual_fifo", "individual_scan")
    ]
    if not rows:
        print("No individual summary rows for wait plot")
        return

    for key, title, filename in (
        ("avg_wait_ms", "Среднее время ожидания", "avg_wait_ms"),
        ("max_wait_ms", "Максимальное время ожидания", "max_wait_ms"),
    ):
        modes = ["fifo", "scan"]
        means = []
        lower = []
        upper = []
        for mode in modes:
            values = [float(row[key]) for row in rows if row["mode"] == mode]
            mean = float(np.mean(values))
            means.append(mean)
            lower.append(mean - float(np.min(values)))
            upper.append(float(np.max(values)) - mean)

        figure, axes = plt.subplots(figsize=(7.5, 4.5))
        colors = ["#4472c4", "#ed7d31"]
        axes.bar(modes, means, yerr=[lower, upper], capsize=6,
                 color=colors, width=0.5)
        axes.set_title(f"{title}: FIFO и SCAN")
        axes.set_ylabel("Время, мс")
        axes.grid(axis="y", alpha=0.3)
        for index, value in enumerate(means):
            axes.text(index, value, f"{value:.2f}", ha="center",
                      va="bottom", fontsize=9)
        figure.tight_layout()
        save_figure(figure, output, filename)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    results_path = args.data / "results.csv"
    waits_path = args.data / "waits.csv"
    if not results_path.exists() or not waits_path.exists():
        print("Run scripts/run_experiments.py first", file=__import__("sys").stderr)
        return 1

    args.output.mkdir(parents=True, exist_ok=True)
    plt.rcParams["font.family"] = "DejaVu Sans"
    results = read_rows(results_path)
    waits = read_rows(waits_path)

    plot_throughput(results, args.output)
    plot_wait_distribution(waits, args.output)
    plot_wait_summary(results, args.output)
    print("Plots saved to", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
