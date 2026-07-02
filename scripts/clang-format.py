#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

# ── Config ───────────────────────────────────────────────────────────────────
# Directories scanned for C/C++ sources to format, relative to the repo root.
SOURCE_DIRS = ("include", "src", "tests")
SOURCE_EXTS = ("*.cpp", "*.h", "*.hpp", "*.c")
CLANG_IMAGE = "silkeh/clang:20"
# ─────────────────────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent

format_cmd = (
    "find " + " ".join(SOURCE_DIRS) + " -type f \\( "
    + " -o ".join(f'-name "{ext}"' for ext in SOURCE_EXTS)
    + " \\) | xargs clang-format -i"
)

try:
    subprocess.run(
        [
            "docker", "run", "--rm",
            "-v", f"{REPO_ROOT}:/src",
            "-w", "/src",
            CLANG_IMAGE,
            "sh", "-c", format_cmd,
        ],
        check=True,
    )
except subprocess.CalledProcessError as exc:
    sys.exit(exc.returncode)
