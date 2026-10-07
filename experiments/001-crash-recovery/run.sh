#!/usr/bin/env bash
set -euo pipefail

SERVICE="resilient-pi.service"
HEALTH_URL="http://127.0.0.1:8080/health"

TRIALS=20
TIMEOUT_MS=10000
POLL_INTERVAL=0.05

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/raw"
RESULTS_FILE="$RESULTS_DIR/crash_recovery.csv"

mkdir -p "$RESULTS_DIR"

# Prevent overlapping experiments.
exec 9>"/tmp/resilient-pi-crash-test.lock"
flock -n 9 || {
    echo "Another crash-recovery experiment is running."
    exit 1
}

now_ms() {
    date +%s%3N
}

get_pid() {
    systemctl show "$SERVICE" \
        --property=MainPID \
        --value
}

is_healthy() {
    curl --silent --show-error --fail \
        --max-time 1 \
        "$HEALTH_URL" >/dev/null 2>&1
}

echo "trial,old_pid,new_pid,recovery_ms,result" > "$RESULTS_FILE"

for ((trial=1; trial<=TRIALS; trial++)); do
    echo "=== Trial $trial/$TRIALS ==="

    old_pid="$(get_pid)"

    if [[ "$old_pid" == "0" ]] || ! is_healthy; then
        echo "Service is not healthy before trial $trial"
        exit 1
    fi

    # Record the timestamp immediately before injecting failure.
    start_ms="$(now_ms)"

    sudo systemctl kill \
        --kill-whom=main \
        --signal=SIGKILL \
        "$SERVICE"

    result="timeout"
    new_pid=0
    recovery_ms=-1

    while true; do
        current_ms="$(now_ms)"
        elapsed_ms=$((current_ms - start_ms))

        if ((elapsed_ms >= TIMEOUT_MS)); then
            break
        fi

        new_pid="$(get_pid)"

        # A new process must exist AND respond successfully.
        if [[ "$new_pid" != "0" &&
              "$new_pid" != "$old_pid" ]] && is_healthy; then
            recovery_ms="$(( $(now_ms) - start_ms ))"
            result="success"
            break
        fi

        sleep "$POLL_INTERVAL"
    done

    echo "$trial,$old_pid,$new_pid,$recovery_ms,$result" \
        >> "$RESULTS_FILE"

    echo "Result: $result | Recovery: ${recovery_ms}ms"

    if [[ "$result" != "success" ]]; then
        echo "Stopping experiment after failed recovery."
        exit 1
    fi

    sleep 1
done

echo
echo "Experiment completed."
echo "Results: $RESULTS_FILE"

