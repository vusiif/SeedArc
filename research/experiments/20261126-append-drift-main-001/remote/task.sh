#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B drift_probe_task.py
