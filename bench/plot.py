#!/usr/bin/env python3
"""실행별 CSV를 반복 중앙값으로 요약하고 같은 조건의 IPC만 비교한다."""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BACKENDS = ("pipe", "sysv_mq", "sysv_shm")
COLORS = dict(zip(BACKENDS, ("#187d90", "#ba6334", "#7655a3")))
KEYS = ("backend", "scenario", "size", "clients", "filter", "depth", "count", "warmup", "mode", "payload_bytes")
METRICS = ("p50_us", "p95_us", "p99_us", "max_us", "mean_us", "mbps", "imgs", "messages_per_s",
           "ru_utime_ms", "ru_stime_ms", "nvcsw", "nivcsw", "rss_start_kib", "rss_end_kib")


def load(path):
    with path.open(newline="") as source:
        groups = defaultdict(list)
        for row in csv.DictReader(source):
            if row["mode"] == "smoke":
                raise ValueError("smoke 자료는 제출 그래프로 사용하지 않습니다")
            groups[tuple(row[k] for k in KEYS)].append(row)
    result = []
    for key, rows in groups.items():
        repeats = [r["repeat"] for r in rows]
        if len(set(repeats)) != len(repeats):
            raise ValueError(f"중복 반복: {key}")
        item = dict(zip(KEYS, key))
        item.update({metric: median(float(r[metric]) for r in rows) for metric in METRICS})
        item["repeats"] = len(rows)
        result.append(item)
    if not result:
        raise ValueError(f"빈 자료: {path}")
    return result


def select(rows, **conditions):
    return [r for r in rows if all(r[k] == str(v) for k, v in conditions.items())]


def comparison(rows, x, metric, xlabel, ylabel, path, log=False, tail=False):
    varying = {"backend", x} | ({"size"} if x == "payload_bytes" else set())
    if any(len({r[k] for r in rows}) != 1 for k in KEYS if k not in varying):
        raise ValueError(f"{path.name}: 서로 다른 고정 조건을 합칠 수 없습니다")
    fig, ax = plt.subplots(figsize=(7.6, 4.6), constrained_layout=True)
    expected = None
    for backend in BACKENDS:
        data = sorted(select(rows, backend=backend), key=lambda r: int(r[x]))
        values = [int(r[x]) for r in data]
        if not values or len(values) != len(set(values)):
            raise ValueError(f"{path.name}: {backend} 비교 조건 누락/중복")
        if expected is not None and values != expected:
            raise ValueError(f"{path.name}: IPC별 x 조건이 다릅니다")
        expected = values
        ax.plot(values, [r[metric] for r in data], "o-", color=COLORS[backend], label=backend + (" p50" if tail else ""))
        if tail:
            ax.plot(values, [r["p99_us"] for r in data], "x--", color=COLORS[backend], label=backend + " p99")
    if log:
        ax.set_xscale("log", base=2)
    ax.set(xlabel=xlabel, ylabel=ylabel)
    ax.grid(alpha=0.2)
    ax.legend(fontsize=8)
    fig.savefig(path, dpi=180)
    plt.close(fig)


def cpu_plot(rows, path):
    data = select(rows, size=512, clients=1, filter="gray", depth=1)
    ordered = []
    for backend in BACKENDS:
        found = select(data, backend=backend)
        if len(found) != 1:
            raise ValueError(f"CPU: {backend}의 S1 512 조건이 필요합니다")
        ordered.append(found[0])
    fig, ax = plt.subplots(figsize=(6.8, 4.6), constrained_layout=True)
    user = [r["ru_utime_ms"] for r in ordered]
    ax.bar(BACKENDS, user, label="user", color="#187d90")
    ax.bar(BACKENDS, [r["ru_stime_ms"] for r in ordered], bottom=user, label="sys", color="#ba6334")
    ax.set(xlabel="IPC", ylabel="CPU time, server + clients (ms)")
    ax.legend()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def improvement(rows, path):
    for backend in BACKENDS:
        pair = select(rows, backend=backend)
        if len(pair) != 2 or any(len({r[k] for r in pair}) != 1 for k in KEYS if k != "mode"):
            raise ValueError("개선 전후에는 mode 외의 조건이 같아야 합니다")
    fig, axes = plt.subplots(1, 3, figsize=(12, 4), constrained_layout=True)
    for ax, metric, label in zip(axes, ("imgs", "p99_us", "rss_end_kib"),
                                 ("Images/s", "p99 RTT (us)", "RSS end sum (KiB)")):
        for offset, mode, color in ((-0.18, "baseline", "#187d90"), (0.18, "inplace", "#ba6334")):
            values = []
            for backend in BACKENDS:
                found = select(rows, backend=backend, mode=mode)
                if len(found) != 1:
                    raise ValueError("개선 비교는 같은 size/clients/depth/count 조건 1쌍이 필요합니다")
                values.append(found[0][metric])
            ax.bar([i + offset for i in range(3)], values, width=0.36, color=color, label=mode)
        ax.set_xticks(range(3), BACKENDS, rotation=15)
        ax.set_ylabel(label)
    axes[0].legend()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    environment = (root / "environment.txt").read_text()
    if "phase=smoke " in environment:
        raise ValueError("smoke 결과는 성능 그래프로 사용하지 않습니다")
    output = root / "graphs"
    output.mkdir(exist_ok=True)
    all_rows = []
    for name in ("basic", "s1", "s2", "s3", "s4", "s5", "improvement"):
        path = root / f"{name}.csv"
        if not path.exists():
            continue
        rows = load(path)
        all_rows.extend(rows)
        if name == "basic":
            comparison(select(rows, clients=1, mode="latency"), "payload_bytes", "p50_us",
                       "Payload (bytes)", "RTT (us)", output / "basic-payload-latency.png", log=True, tail=True)
            comparison(select(rows, clients=1, mode="throughput"), "payload_bytes", "mbps",
                       "Payload (bytes)", "One-way payload (MB/s)", output / "basic-payload-throughput.png", log=True)
            comparison(select(rows, clients=1, mode="throughput"), "payload_bytes", "messages_per_s",
                       "Payload (bytes)", "Messages/s", output / "basic-payload-messages.png", log=True)
            sizes = sorted({int(r["payload_bytes"]) for r in rows})
            for size in sizes:
                comparison(select(rows, payload_bytes=size, mode="latency"), "clients", "p50_us",
                           "Concurrent clients", "RTT (us)", output / f"basic-clients-{size}-latency.png", tail=True)
                comparison(select(rows, payload_bytes=size, mode="throughput"), "clients", "mbps",
                           "Concurrent clients", "One-way payload (MB/s)", output / f"basic-clients-{size}-throughput.png")
        elif name == "s1":
            comparison(rows, "payload_bytes", "p50_us", "Payload (bytes)", "RTT (us)",
                       output / "app-payload-latency.png", log=True, tail=True)
            comparison(rows, "payload_bytes", "imgs", "Payload (bytes)", "Images/s",
                       output / "app-payload-throughput.png", log=True)
            cpu_plot(rows, output / "app-cpu.png")
        elif name == "s2":
            comparison(rows, "clients", "imgs", "Concurrent clients", "Images/s", output / "app-clients-throughput.png")
        elif name == "improvement":
            improvement(rows, output / "improvement.png")
    if not all_rows:
        raise ValueError("집계 CSV가 없습니다")
    with (root / "median.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=(*KEYS, *METRICS, "repeats"))
        writer.writeheader()
        writer.writerows(all_rows)
    print(output)


if __name__ == "__main__":
    main()
