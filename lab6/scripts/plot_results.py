#!/usr/bin/env python3

import csv
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "data" / "raw" / "experiment_raw.csv"
PLOTS = ROOT / "data" / "plots"

VALUE_CONFIGS = ["values_1", "values_10", "values_100", "values_500"]
CHANGE_CONFIGS = ["change_1", "change_5", "change_10"]
SCENARIO_CONFIGS = ["change_1", "change_5", "change_10", "delete_1"]

SCENARIO_LABELS = {
    "change_1": "1 значение",
    "change_5": "5 значений",
    "change_10": "10 значений",
    "delete_1": "удаление",
}


def read_rows():
    with RAW.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        row["read_ms"] = float(row["read_ms"])
        row["latency_ms"] = float(row["latency_ms"])
        row["detected_total"] = int(row["detected_total"])
        row["notifications"] = int(row["notifications"])
        row["requested_changes"] = int(row["requested_changes"])
        row["initial_values"] = int(row["initial_values"])
    return rows


def group(rows, config):
    return [row for row in rows if row["config"] == config]


def mean(values):
    return statistics.mean(values)


def save(figure, name):
    figure.tight_layout()
    figure.savefig(PLOTS / f"{name}.png", dpi=170, bbox_inches="tight")
    figure.savefig(PLOTS / f"{name}.pdf", bbox_inches="tight")
    plt.close(figure)
    print(f"written {PLOTS / name}.png")


def plot_read_time(rows):
    counts = []
    means = []
    low = []
    high = []
    for config in VALUE_CONFIGS:
        group_rows = group(rows, config)
        values = [row["read_ms"] for row in group_rows]
        counts.append(group_rows[0]["initial_values"])
        means.append(mean(values))
        low.append(mean(values) - min(values))
        high.append(max(values) - mean(values))

    figure, axes = plt.subplots(figsize=(7.2, 4.4))
    axes.errorbar(
        counts, means, yerr=[low, high], marker="o", capsize=4,
        linewidth=1.8, color="#2f5597",
    )
    axes.set_xscale("log")
    axes.set_xticks(counts)
    axes.set_xticklabels([str(value) for value in counts])
    axes.set_xlabel("Количество значений в наблюдаемом разделе")
    axes.set_ylabel("Время повторного чтения, мс")
    axes.set_title("Зависимость времени чтения раздела от числа значений")
    axes.grid(True, linestyle="--", alpha=0.4)
    save(figure, "read_time_vs_values")


def plot_detection_latency(rows):
    labels = []
    means = []
    low = []
    high = []
    for config in CHANGE_CONFIGS + ["delete_1"]:
        group_rows = group(rows, config)
        values = [row["latency_ms"] for row in group_rows]
        labels.append(SCENARIO_LABELS[config])
        means.append(mean(values))
        low.append(mean(values) - min(values))
        high.append(max(values) - mean(values))

    figure, axes = plt.subplots(figsize=(7.2, 4.4))
    positions = list(range(len(labels)))
    axes.bar(positions, means, color="#4472c4", width=0.55)
    axes.errorbar(positions, means, yerr=[low, high], fmt="none",
                  ecolor="#1f3864", capsize=5, linewidth=1.2)
    axes.set_xticks(positions)
    axes.set_xticklabels(labels)
    axes.set_xlabel("Сценарий изменения")
    axes.set_ylabel("Задержка уведомления, мс")
    axes.set_title("Задержка обнаружения изменения в inotify")
    axes.grid(True, axis="y", linestyle="--", alpha=0.4)
    save(figure, "detection_latency")


def plot_detected_changes(rows):
    labels = []
    requested = []
    detected = []
    notifications = []
    for config in SCENARIO_CONFIGS:
        group_rows = group(rows, config)
        labels.append(SCENARIO_LABELS[config])
        requested.append(mean([row["requested_changes"] for row in group_rows]))
        detected.append(mean([row["detected_total"] for row in group_rows]))
        notifications.append(mean([row["notifications"] for row in group_rows]))

    positions = list(range(len(labels)))
    width = 0.26
    figure, axes = plt.subplots(figsize=(7.6, 4.4))
    axes.bar([p - width for p in positions], requested, width,
             label="Запрошено изменений", color="#a5a5a5")
    axes.bar(positions, detected, width,
             label="Обнаружено изменений", color="#4472c4")
    axes.bar([p + width for p in positions], notifications, width,
             label="Уведомлений", color="#ed7d31")
    axes.set_xticks(positions)
    axes.set_xticklabels(labels)
    axes.set_xlabel("Сценарий эксперимента")
    axes.set_ylabel("Количество")
    axes.set_title("Обнаруженные изменения и число уведомлений")
    axes.legend()
    axes.grid(True, axis="y", linestyle="--", alpha=0.4)
    save(figure, "detected_changes")


def main():
    if not RAW.exists():
        raise SystemExit(f"raw data not found: {RAW}")
    PLOTS.mkdir(parents=True, exist_ok=True)
    plt.rcParams["font.family"] = "DejaVu Sans"
    plt.rcParams["axes.unicode_minus"] = False
    rows = read_rows()
    plot_read_time(rows)
    plot_detection_latency(rows)
    plot_detected_changes(rows)


if __name__ == "__main__":
    main()
