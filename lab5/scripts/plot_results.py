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
DEFAULT_DATA = ROOT / "data" / "raw"
DEFAULT_OUTPUT = ROOT / "data" / "plots"

LABELS = {
    "chain3": "3 узла",
    "chain4": "4 узла",
}


def read_rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def save_figure(figure, output, name):
    output.mkdir(parents=True, exist_ok=True)
    figure.savefig(output / f"{name}.png", dpi=160, bbox_inches="tight")
    figure.savefig(output / f"{name}.pdf", bbox_inches="tight")
    plt.close(figure)


def group_by_config(rows, field):
    grouped = defaultdict(list)
    for row in rows:
        grouped[row["config"]].append(float(row[field]))
    return grouped


def plot_latency(rows, output):
    grouped = group_by_config(rows, "avg_latency_ms")
    configurations = sorted(grouped)
    means = [float(np.mean(grouped[config])) for config in configurations]
    lower = [
        mean - float(np.min(grouped[config]))
        for mean, config in zip(means, configurations)
    ]
    upper = [
        float(np.max(grouped[config])) - mean
        for mean, config in zip(means, configurations)
    ]

    figure, axes = plt.subplots(figsize=(7.2, 4.4))
    positions = np.arange(len(configurations))
    axes.bar(
        positions,
        means,
        yerr=[lower, upper],
        capsize=6,
        color="#4472c4",
        edgecolor="#2f5597",
        width=0.55,
    )
    axes.set_xticks(positions)
    axes.set_xticklabels([LABELS.get(config, config)
                          for config in configurations])
    axes.set_xlabel("Конфигурация сети")
    axes.set_ylabel("Средняя задержка доставки, мс")
    axes.set_title("Задержка доставки через промежуточные узлы")
    axes.grid(axis="y", alpha=0.3)
    for position, mean in zip(positions, means):
        axes.text(position, mean, f"{mean:.3f}", ha="center",
                  va="bottom", fontsize=9)
    figure.tight_layout()
    save_figure(figure, output, "latency_vs_nodes")


def plot_throughput(rows, output):
    grouped = group_by_config(rows, "throughput_msg_s")
    configurations = sorted(grouped)
    means = [float(np.mean(grouped[config])) for config in configurations]
    lower = [
        mean - float(np.min(grouped[config]))
        for mean, config in zip(means, configurations)
    ]
    upper = [
        float(np.max(grouped[config])) - mean
        for mean, config in zip(means, configurations)
    ]

    figure, axes = plt.subplots(figsize=(7.2, 4.4))
    positions = np.arange(len(configurations))
    axes.bar(
        positions,
        means,
        yerr=[lower, upper],
        capsize=6,
        color="#70ad47",
        edgecolor="#507e32",
        width=0.55,
    )
    axes.set_xticks(positions)
    axes.set_xticklabels([LABELS.get(config, config)
                          for config in configurations])
    axes.set_xlabel("Конфигурация сети")
    axes.set_ylabel("Пропускная способность, сообщений/с")
    axes.set_title("Пропускная способность при отправке 20 сообщений")
    axes.grid(axis="y", alpha=0.3)
    for position, mean in zip(positions, means):
        axes.text(position, mean, f"{mean:.2f}", ha="center",
                  va="bottom", fontsize=9)
    figure.tight_layout()
    save_figure(figure, output, "throughput_vs_nodes")


def plot_forwarding(rows, output):
    grouped = defaultdict(lambda: defaultdict(list))
    for row in rows:
        grouped[row["config"]]["delivered"].append(
            float(row["messages_delivered"])
        )
        grouped[row["config"]]["forwarded"].append(
            float(row["forwarded"])
        )

    configurations = sorted(grouped)
    positions = np.arange(len(configurations))
    width = 0.36

    delivered = [
        float(np.mean(grouped[config]["delivered"]))
        for config in configurations
    ]
    forwarded = [
        float(np.mean(grouped[config]["forwarded"]))
        for config in configurations
    ]

    figure, axes = plt.subplots(figsize=(7.2, 4.4))
    axes.bar(positions - width / 2, delivered, width,
             label="Доставлено получателю", color="#4472c4")
    axes.bar(positions + width / 2, forwarded, width,
             label="Пересылок через промежуточные узлы", color="#ed7d31")
    axes.set_xticks(positions)
    axes.set_xticklabels([LABELS.get(config, config)
                          for config in configurations])
    axes.set_xlabel("Конфигурация сети")
    axes.set_ylabel("Количество сообщений")
    axes.set_title("Доставка и пересылка сообщений")
    axes.grid(axis="y", alpha=0.3)
    axes.legend()
    for position, value in zip(positions - width / 2, delivered):
        axes.text(position, value, f"{value:.0f}", ha="center",
                  va="bottom", fontsize=8)
    for position, value in zip(positions + width / 2, forwarded):
        axes.text(position, value, f"{value:.0f}", ha="center",
                  va="bottom", fontsize=8)
    figure.tight_layout()
    save_figure(figure, output, "messages_vs_nodes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    results_path = args.data / "experiment_raw.csv"
    if not results_path.exists():
        print("Run scripts/run_experiments.py first")
        return 1

    plt.rcParams["font.family"] = "DejaVu Sans"
    rows = read_rows(results_path)
    plot_latency(rows, args.output)
    plot_throughput(rows, args.output)
    plot_forwarding(rows, args.output)
    print(f"plots written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
