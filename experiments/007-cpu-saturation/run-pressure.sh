
#!/usr/bin/env bash
set -Eeuo pipefail

# Usage: ./run-pressure.sh <0|1|2|3|4> <trial-id>
# Execute on the Raspberry Pi.
# Start wrk from Arch while this script is active.

WORKERS="${1:?Usage: $0 <workers 0-4> <trial-id>}"
TRIAL="${2:?Provide trial-id}"

if ! [[ "$WORKERS" =~ ^[0-4]$ ]]; then
    echo "Worker count must be 0, 1, 2, 3, or 4" >&2
    exit 1
fi

OUT="results/raw/day09/cpu${WORKERS}/${TRIAL}"
mkdir -p "$OUT"

echo "Output directory: $OUT"
date -Is | tee "$OUT/start-time.txt"

# Record system details.
{
    uname -a
    nproc
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    free -h
} > "$OUT/environment.txt"

PIDS=()

cleanup() {
    trap - EXIT INT TERM

    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done

    for pid in "${PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    date -Is > "$OUT/end-time.txt"
}
trap cleanup EXIT INT TERM

# Sample temperature, frequency, and load once per second.
(
    while true; do
        TS=$(date +%s)

        TEMP=$(awk '{print $1/1000}' \
            /sys/class/thermal/thermal_zone0/temp)

        FREQ=$(cat \
            /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)

        LOAD=$(cut -d' ' -f1 /proc/loadavg)

        printf '%s,%s,%s,%s\n' \
            "$TS" "$TEMP" "$FREQ" "$LOAD"

        sleep 1
    done
) > "$OUT/thermal.csv" &
PIDS+=("$!")

echo 'timestamp,temp_c,cpu0_freq_khz,load1' \
  > "$OUT/thermal-header.txt"

mpstat -P ALL 1 > "$OUT/mpstat.txt" &
PIDS+=("$!")

vmstat 1 > "$OUT/vmstat.txt" &
PIDS+=("$!")

# Stress process remains active while load is generated.
if (( WORKERS > 0 )); then
    stress-ng \
        --cpu "$WORKERS" \
        --cpu-method matrixprod \
        --cpu-load 100 \
        --timeout 35s \
        --metrics-brief \
        > "$OUT/stress-ng.txt" 2>&1 &
    PIDS+=("$!")
else
    sleep 35 &
    PIDS+=("$!")
fi

echo
echo "READY: CPU pressure level $WORKERS"
echo "Start wrk on Arch NOW."
echo "Sampling for 35 seconds..."
echo

sleep 35

echo "Experiment finished."
