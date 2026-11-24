#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
# Reuse the exact Release binary that passed 43/43 in the cancelled dev pilot.
# Read the prior task only; do not restart it or change its status / results.
prior=/home/sc/long-tasks/20261123T123105Z-d5596074/work
export LD_LIBRARY_PATH="$prior/toolchain/usr/lib/x86_64-linux-gnu"
export LC_ALL=C.UTF-8
trap 'if [ -d "$TASK_WORK/history" ]; then cp -R -- "$TASK_WORK/history" "$TASK_RESULTS/history"; fi' EXIT
python3 check_append_history_r2.py --tool "$prior/build/research/tools/seedarc_append4_study" --work "$TASK_WORK/history"
