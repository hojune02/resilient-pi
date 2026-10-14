#!/usr/bin/env python3
"""Allocate and touch 192 MiB of memory for cgroup testing."""

import os
import time

MIB = 1024 * 1024
import sys

ALLOCATION = int(sys.argv[1]) * MIB if len(sys.argv) > 1 else 192 * MIB
PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")

print(f"PID: {os.getpid()}", flush=True)

data = bytearray(ALLOCATION)

# Explicitly write to each page so memory is committed.
for offset in range(0, ALLOCATION, PAGE_SIZE):
    data[offset] = 1

print(
    f"Touched {ALLOCATION // MIB} MiB of memory",
    flush=True,
)

with open("/proc/self/status", encoding="utf-8") as f:
    for line in f:
        if line.startswith(("VmSize:", "VmRSS:", "VmHWM:")):
            print(line.strip(), flush=True)

# Hold the allocation long enough for inspection.
time.sleep(20)

print("Allocation test completed", flush=True)
