
#!/usr/bin/env python3

import csv
from pathlib import Path
from statistics import mean

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = ROOT / "results/raw/day04/baseline.csv"
OUT_DIR = ROOT / "results/figures"

OUT_DIR.mkdir(parents=True, exist_ok=True)

with CSV_PATH.open(newline="") as f:
    rows = list(csv.DictReader(f))

# Group repeated trials by concurrency.
groups = {}
for row in rows:
    c = int(row["connections"])
    groups.setdefault(c, []).append({
        "rps": float(row["requests_per_sec"]),
        "p50": float(row["p50_ms"]),
        "p99": float(row["p99_ms"]),
    })

connections = sorted(groups)

# ---- Figure 1: Throughput ----

fig, ax = plt.subplots(figsize=(8, 5))

means = [
    mean(r["rps"] for r in groups[c])
    for c in connections
]

ax.plot(connections, means, marker="o",
        label="Mean throughput")

for c in connections:
    for result in groups[c]:
        ax.scatter(c, result["rps"], alpha=0.5)

ax.set_title("Resilient Pi: Throughput vs Concurrency")
ax.set_xlabel("Concurrent connections")
ax.set_ylabel("Requests per second")
ax.grid(True, alpha=0.25)
ax.legend()

fig.tight_layout()
fig.savefig(OUT_DIR / "throughput-vs-concurrency.png",
            dpi=180)
plt.close(fig)

# ---- Figure 2: Latency ----

fig, ax = plt.subplots(figsize=(8, 5))

for metric, label in [
    ("p50", "p50"),
    ("p99", "p99"),
]:
    values = [
        mean(r[metric] for r in groups[c])
        for c in connections
    ]

    ax.plot(connections, values,
            marker="o", label=f"Mean trial {label}")

    for c in connections:
        for result in groups[c]:
            ax.scatter(c, result[metric], alpha=0.45)

ax.set_title("Resilient Pi: Latency vs Concurrency")
ax.set_xlabel("Concurrent connections")
ax.set_ylabel("Latency (ms)")
ax.grid(True, alpha=0.25)
ax.legend()

fig.tight_layout()
fig.savefig(OUT_DIR / "latency-vs-concurrency.png",
            dpi=180)
plt.close(fig)

print("Figures generated:")
for name in [
    "throughput-vs-concurrency.png",
    "latency-vs-concurrency.png",
]:
    print(OUT_DIR / name)
