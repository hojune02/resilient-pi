import csv
import glob
import statistics

files = sorted(glob.glob("runs/*/results.csv"))
if not files:
    raise SystemExit("No results.csv files found")

path = files[-1]
print(f"Analysing: {path}\n")

with open(path, newline="") as f:
    rows = list(csv.DictReader(f))

for network in ["LAN", "TAILSCALE"]:
    group = [r for r in rows if r["network"] == network]

    print(f"{network} (n={len(group)})")

    for field in ["requests_per_sec", "p50_ms", "p99_ms"]:
        values = [float(r[field]) for r in group]

        mean = statistics.mean(values)
        sd = statistics.stdev(values) if len(values) > 1 else 0

        print(
            f"  {field:20} "
            f"mean={mean:9.2f}  "
            f"sd={sd:8.2f}  "
            f"min={min(values):8.2f}  "
            f"max={max(values):8.2f}"
        )

    print()
