#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
python3 -B codec_probe_task.py framed
