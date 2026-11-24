#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
export LD_LIBRARY_PATH=/home/sc/long-tasks/20261123T123105Z-d5596074/work/toolchain/usr/lib/x86_64-linux-gnu
export LC_ALL=C.UTF-8
python3 -B append_large_continue.py
