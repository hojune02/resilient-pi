
#!/usr/bin/env python3
"""Timestamped HTTP availability probe for controlled failures."""

import argparse
import csv
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--url", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--duration", type=float, default=30)
parser.add_argument("--interval", type=float, default=0.2)
parser.add_argument("--timeout", type=float, default=1.0)
args = parser.parse_args()

if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
    parser.error("duration, interval and timeout must be positive")

output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)

fields = [
    "timestamp_utc",
    "epoch_ns",
    "elapsed_ms",
    "http_status",
    "error",
]

deadline = time.monotonic() + args.duration

with output.open("w", newline="") as file:
    writer = csv.DictWriter(file, fieldnames=fields)
    writer.writeheader()

    while time.monotonic() < deadline:
        started = time.perf_counter()
        timestamp = datetime.now(timezone.utc).isoformat()
        epoch_ns = time.time_ns()

        status = ""
        error = ""

        try:
            request = urllib.request.Request(
                args.url,
                headers={"Connection": "close"},
            )
            with urllib.request.urlopen(
                request, timeout=args.timeout
            ) as response:
                status = response.status
                response.read()

        except urllib.error.HTTPError as exc:
            status = exc.code
            error = f"HTTP {exc.code}"
            exc.close()

        except Exception as exc:
            error = type(exc).__name__

        elapsed_ms = (time.perf_counter() - started) * 1000

        writer.writerow({
            "timestamp_utc": timestamp,
            "epoch_ns": epoch_ns,
            "elapsed_ms": round(elapsed_ms, 3),
            "http_status": status,
            "error": error,
        })
        file.flush()

        print(
            f"{timestamp} "
            f"status={status or '-'} "
            f"latency={elapsed_ms:.1f}ms "
            f"error={error or '-'}",
            flush=True,
        )

        time.sleep(args.interval)
