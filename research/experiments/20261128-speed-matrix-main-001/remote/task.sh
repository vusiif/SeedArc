#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B speed_matrix_task.py
