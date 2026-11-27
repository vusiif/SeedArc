#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B parallel_frames_task.py
