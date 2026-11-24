#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B solid_version_task.py
