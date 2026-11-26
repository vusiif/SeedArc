#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B cross_cpu_india_task.py
