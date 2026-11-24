#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
prior=/home/sc/long-tasks/20261123T123105Z-d5596074/work
tested=/home/sc/long-tasks/20261123T134340Z-d28b6277/work/release/research/tools/seedarc_append4_study
export LD_LIBRARY_PATH="$prior/toolchain/usr/lib/x86_64-linux-gnu"
export LC_ALL=C.UTF-8
sha256sum "$tested" > "$TASK_RESULTS/binary-sha256.txt"
python3 append_resource_probe.py --tool "$tested" --work "$TASK_WORK/probe" --results "$TASK_RESULTS/after"
