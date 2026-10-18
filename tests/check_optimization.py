"""Verify effective application optimization: python tests/check_optimization.py [build/tab5]."""
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build/tab5"
commands = json.loads((build / "compile_commands.json").read_text())
checked = 0
workarounds = 0
failures = []
# ESP-IDF esp_libc/CMakeLists.txt pins these to -O2 for GCC-419 / IDF-13089.
# Preserve upstream source-specific correctness constraints over global tuning.
libc_workarounds = {
    "components/esp_libc/src/port/riscv/memcpy.c",
    "components/esp_libc/src/port/riscv/memmove.c",
    "components/esp_libc/src/string/memcmp.c",
    "components/esp_libc/src/port/riscv/strcpy.c",
    "components/esp_libc/src/string/strncpy.c",
    "components/esp_libc/src/string/strncmp.c",
}
for entry in commands:
    source = Path(entry["file"])
    if source.suffix.lower() not in {".c", ".cpp", ".cc", ".cxx"}:
        continue
    # Toolchain identification probes are not application build targets.
    if "CMakeFiles" in source.parts and any(part.startswith("CompilerId") for part in source.parts):
        continue
    command = entry.get("command", " ".join(entry.get("arguments", [])))
    levels = re.findall(r"(?<!\S)-O(?:[0-3gsz]|fast)(?!\S)", command)
    is_workaround = any(source.as_posix().endswith(path) for path in libc_workarounds)
    if levels and levels[-1] == "-O2" and is_workaround:
        workarounds += 1
    elif not levels or levels[-1] != "-O3":
        failures.append(f"{source}: effective optimization {levels[-1] if levels else 'unset'}")
    checked += 1

if not checked:
    raise SystemExit("No application C/C++ compilation commands found")
if failures:
    raise SystemExit("\n".join(failures))
print(f"Optimization OK: {checked - workarounds} C/C++ units use -O3; "
      f"{workarounds} retain ESP-IDF's source-specific -O2 compiler workaround")
