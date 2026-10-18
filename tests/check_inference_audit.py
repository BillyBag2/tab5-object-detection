"""Validate frozen-input board tests and compare deterministic inputs across builds.

Usage: python tests/check_inference_audit.py first.log [second.log ...]
Camera fixtures are frozen within each boot, not assumed identical across boots.
"""
from pathlib import Path
import re
import sys


def check(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    assert "DONE failures=0" in text, f"{path}: audit did not finish successfully"
    assert not re.search(r"Guru Meditation|CORRUPT HEAP|assert failed|Task watchdog got triggered", text), path
    tensors = [line for line in text.splitlines() if "inference_audit: tensor=" in line]
    assert len(tensors) == 6 and all("simd_aligned=1" in line for line in tensors), f"{path}: tensor alignment"
    rows = []
    for line in text.splitlines():
        if "inference_audit: fixture=" in line and "repeat=" in line:
            rows.append(dict(re.findall(r"(\w+)=([\w.-]+)", line)))
    fingerprints = ("raw", "input", "output", "results", "objects")
    fixtures = {row["fixture"] for row in rows}
    assert {"pattern", "camera"} <= fixtures, f"{path}: missing fixtures"
    for fixture in sorted(fixtures):
        group = [row for row in rows if row["fixture"] == fixture]
        assert [row["repeat"] for row in group] == [str(i) for i in range(6)], f"{path}: {fixture} incomplete"
        for row in group:
            assert all(row[key] == "1" for key in ("pass", "heap", "guards")), f"{path}: integrity failure {row}"
            assert row["output"] == row["post"] == row["settled"], f"{path}: output changed outside inference"
            assert all(row[key] == group[0][key] for key in fingerprints), f"{path}: repeat changed {fixture}"
    for fixture in ("pattern", "camera"):
        if fixture + "_no_switch" in fixtures:
            normal = next(row for row in rows if row["fixture"] == fixture)
            suspended = next(row for row in rows if row["fixture"] == fixture + "_no_switch")
            assert all(normal[key] == suspended[key] for key in fingerprints), f"{path}: scheduler comparison differs {fixture}"
    pattern = next(row for row in rows if row["fixture"] == "pattern")
    assert pattern["raw"] == "d84f4f2c", f"{path}: pattern differs from independently generated host fixture"
    print(f"{path}: {len(rows)} repeated runs passed; L2={pattern['l2']} line={pattern['line']}; pattern output={pattern['output']}")
    return tuple(pattern[key] for key in fingerprints)


if __name__ == "__main__":
    try:
        assert len(sys.argv) > 1, __doc__
        fingerprints = [check(path) for path in sys.argv[1:]]
        assert all(value == fingerprints[0] for value in fingerprints), "Deterministic fixture differs across builds"
        if len(fingerprints) > 1:
            print("Cross-build deterministic image, preprocessed tensor, raw outputs and detections match exactly (CRC32).")
    except (AssertionError, OSError) as error:
        raise SystemExit(str(error))
