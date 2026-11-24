#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B bounded_repeat_task.py
