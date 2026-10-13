
#!/usr/bin/env python3
"""Compare read-only HTTP latency: persistent vs observable server."""

import csv
import math
import statistics
import time
import urllib.error
import urllib.request
from pathlib import Path

ROUNDS = 6
REQUESTS_PER_ROUND = 100
WARMUP_REQUESTS = 20

SERVERS = {
    "persistent": "http://127.0.0.1:8081/counter",
    "observable": "http://127.0.0.1:8082/counter",
}

OUTPUT = Path("results/raw/day08/overhead-latency.csv")


def request(url):
    start = time.perf_counter_ns()

    with urllib.request.urlopen(url, timeout=5) as response:
        body = response.read()
        status = response.status

    elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000

    if status != 200 or not body.strip().isdigit():
        raise RuntimeError(
            f"Unexpected response: status={status}, body={body!r}"
        )

    return elapsed_ms


def percentile(samples, fraction):
    ordered = sorted(samples)
    index = max(0, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def main():
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    print("Warming up both servers...")
    for url in SERVERS.values():
        for _ in range(WARMUP_REQUESTS):
            request(url)

    rows = []

    for round_number in range(1, ROUNDS + 1):
        names = list(SERVERS)

        # Alternate test order to reduce systematic order bias.
        if round_number % 2 == 0:
            names.reverse()

        for name in names:
            samples = [
                request(SERVERS[name])
                for _ in range(REQUESTS_PER_ROUND)
            ]

            row = {
                "round": round_number,
                "server": name,
                "requests": len(samples),
                "mean_ms": statistics.mean(samples),
                "median_ms": statistics.median(samples),
                "p95_ms": percentile(samples, 0.95),
                "max_ms": max(samples),
            }

            rows.append(row)

            print(
                f"Round {round_number}: {name:10s} "
                f"mean={row['mean_ms']:.3f} ms "
                f"p95={row['p95_ms']:.3f} ms"
            )

    with OUTPUT.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    print("\nRound-level summary:")
    for name in SERVERS:
        means = [
            row["mean_ms"]
            for row in rows
            if row["server"] == name
        ]

        print(
            f"{name}: "
            f"mean of round means={statistics.mean(means):.3f} ms, "
            f"round-mean stdev={statistics.stdev(means):.3f} ms"
        )

    print(f"\nSaved: {OUTPUT}")


if __name__ == "__main__":
    main()
