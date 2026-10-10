#!/usr/bin/env bash
set -euo pipefail

HOST="110.76.76.127"
URL="http://${HOST}:8080/health"

THREADS=2
CONNECTIONS="${1:-10}"
REPETITIONS=3
DURATION=15
WARMUP=5

mkdir -p raw

CSV="baseline.csv"

if [[ ! -f "$CSV" ]]; then
    echo "timestamp,threads,connections,trial,requests_per_sec,p50_ms,p99_ms,errors,timeouts" > "$CSV"
fi

for ((trial=1; trial<=REPETITIONS; trial++)); do
    echo "=== Trial $trial/$REPETITIONS ==="

    echo "Warming up..."
    wrk -t"$THREADS" -c"$CONNECTIONS" \
        -d"${WARMUP}s" --timeout 2s "$URL" \
        >/dev/null

    echo "Measuring..."

    OUTPUT="raw/c${CONNECTIONS}_trial${trial}.txt"

    wrk -t"$THREADS" -c"$CONNECTIONS" \
        -d"${DURATION}s" --latency --timeout 2s "$URL" \
        | tee "$OUTPUT"

    python - "$OUTPUT" "$CSV" "$THREADS" \
        "$CONNECTIONS" "$trial" <<'PY'
import sys
import re
import csv
from datetime import datetime

path, csv_path, threads, connections, trial = sys.argv[1:]

with open(path) as f:
    output = f.read()

def extract(pattern, default=""):
    match = re.search(pattern, output, re.MULTILINE)
    return match.group(1) if match else default

rps = extract(r"Requests/sec:\s+([\d.]+)")
p50 = extract(r"^\s*50%\s+([\d.]+m?s|[\d.]+us)")
p99 = extract(r"^\s*99%\s+([\d.]+m?s|[\d.]+us)")

def to_ms(value):
    if value.endswith("us"):
        return float(value[:-2]) / 1000
    if value.endswith("ms"):
        return float(value[:-2])
    if value.endswith("s"):
        return float(value[:-1]) * 1000
    return None

errors_match = re.search(
    r"Socket errors: connect (\d+), read (\d+), "
    r"write (\d+), timeout (\d+)",
    output
)

if errors_match:
    connect, read, write, timeouts = map(int, errors_match.groups())
    errors = connect + read + write
else:
    errors = 0
    timeouts = 0

row = [
    datetime.now().isoformat(),
    threads,
    connections,
    trial,
    rps,
    to_ms(p50),
    to_ms(p99),
    errors,
    timeouts,
]

with open(csv_path, "a", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(row)

print("Recorded:", row)
PY

    if ((trial < REPETITIONS)); then
        sleep 10
    fi
done

echo "All trials completed."
echo "Results saved in $CSV"

