#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B bounded_extract_task.py
