#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B append_large_task.py
