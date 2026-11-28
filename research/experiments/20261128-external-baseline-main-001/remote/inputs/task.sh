#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B external_baseline_task.py
