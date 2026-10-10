#!/usr/bin/env bash
set -euo pipefail

LAN_URL="http://192.168.0.57:8080/health"
TAILSCALE_URL="http://100.92.165.63:8080/health"

THREADS=2
CONNECTIONS=25
DURATION=15
WARMUP=5
COOLDOWN=10

RUN_ID="$(date +%Y%m%d_%H%M%S)"
OUT="runs/$RUN_ID"
mkdir -p "$OUT"

CSV="$OUT/results.csv"

echo "order,network,rep,requests_per_sec,p50_ms,p99_ms,errors,timeouts" > "$CSV"

# Balanced order: A B B A A B
NETWORKS=(LAN TAILSCALE TAILSCALE LAN LAN TAILSCALE)

lan_rep=0
tailscale_rep=0

for i in "${!NETWORKS[@]}"; do
    network="${NETWORKS[$i]}"
    order=$((i + 1))

    if [[ "$network" == "LAN" ]]; then
        URL="$LAN_URL"
        lan_rep=$((lan_rep + 1))
        rep="$lan_rep"
    else
        URL="$TAILSCALE_URL"
        tailscale_rep=$((tailscale_rep + 1))
        rep="$tailscale_rep"
    fi

    echo "==================================="
    echo "Trial $order: $network (rep $rep)"
    echo "Target: $URL"
    echo "==================================="

    # Warm-up, not recorded
    wrk -t"$THREADS" -c"$CONNECTIONS" \
        -d"${WARMUP}s" \
        --timeout 2s "$URL" >/dev/null

    OUTPUT="$OUT/${order}_${network}_rep${rep}.txt"

    # Recorded measurement
    wrk -t"$THREADS" -c"$CONNECTIONS" \
        -d"${DURATION}s" \
        --latency --timeout 2s "$URL" \
        | tee "$OUTPUT"

    python3 - "$OUTPUT" "$CSV" "$order" "$network" "$rep" <<'PY'
import csv
import re
import sys

path, csv_path, order, network, rep = sys.argv[1:]

with open(path, encoding="utf-8") as f:
    output = f.read()

def extract(pattern):
    match = re.search(pattern, output, re.MULTILINE)
    if not match:
        raise ValueError(f"Missing benchmark field: {pattern}")
    return match.group(1)

def to_ms(value):
    if value.endswith("us"):
        return float(value[:-2]) / 1000
    if value.endswith("ms"):
        return float(value[:-2])
    if value.endswith("s"):
        return float(value[:-1]) * 1000
    raise ValueError(f"Unknown time unit: {value}")

latency_unit = r"[\d.]+(?:us|ms|s)"

rps = float(extract(r"Requests/sec:\s+([\d.]+)"))
p50 = to_ms(extract(r"^\s*50%\s+(" + latency_unit + r")"))
p99 = to_ms(extract(r"^\s*99%\s+(" + latency_unit + r")"))

socket_errors = re.search(
    r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)",
    output
)

if socket_errors:
    connect, read, write, timeouts = map(int, socket_errors.groups())
    errors = connect + read + write
else:
    errors = 0
    timeouts = 0

# wrk may also report HTTP status errors separately.
status_errors = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", output)
if status_errors:
    raise ValueError(
        "Non-success HTTP responses reported; inspect raw output before analysis."
    )

with open(csv_path, "a", newline="") as f:
    writer = csv.writer(f)
    writer.writerow([
        order, network, rep, rps, p50, p99, errors, timeouts
    ])

print(f"Recorded trial {order}: {rps:.2f} req/s")
PY

    if (( order < ${#NETWORKS[@]} )); then
        echo "Cooling down for ${COOLDOWN}s..."
        sleep "$COOLDOWN"
    fi
done

echo
echo "Completed."
echo "Results: $CSV"
