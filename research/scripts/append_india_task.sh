#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
export OMP_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=1
export LC_ALL=C.UTF-8
python3 append_india_bootstrap.py
