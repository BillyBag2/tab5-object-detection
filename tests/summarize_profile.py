"""Summarize serial PROFILE lines: python tests/summarize_profile.py capture.log.

The first (cold) inference of each boot is excluded. Durations are elapsed wall
time, including preemption by other tasks. Input can also be piped via stdin.
"""
import math
from pathlib import Path
import re
import statistics
import sys


def summarize(lines):
    samples = []
    for line in lines:
        if not re.search(r"PROFILE #\d+:", line):
            continue
        values = dict((key, float(value)) for key, value in
                      re.findall(r"(preview|ppa|wait|pre|model|post|total)=([0-9.]+)", line))
        if len(values) == 7:
            samples.append(values)
    if not samples:
        raise ValueError("No warm PROFILE samples found")
    print(f"{len(samples)} warm inference samples; elapsed milliseconds")
    print(f"{'stage':<10} {'mean':>10} {'median':>10} {'p95':>10} {'max':>10}")
    for key in ("preview", "ppa", "wait", "pre", "model", "post", "total"):
        values = sorted(sample[key] for sample in samples)
        p95 = values[math.ceil(0.95 * len(values)) - 1]
        print(f"{key:<10} {statistics.mean(values):10.3f} {statistics.median(values):10.3f} "
              f"{p95:10.3f} {values[-1]:10.3f}")


if __name__ == "__main__":
    try:
        if len(sys.argv) > 1:
            with Path(sys.argv[1]).open(encoding="utf-8", errors="replace") as stream:
                summarize(stream)
        else:
            summarize(sys.stdin)
    except ValueError as error:
        raise SystemExit(str(error))
