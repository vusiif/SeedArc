#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
sha256sum xcpu-tool.tar.gz xcpu-data.tar.xz seedarc_append4_study.us2-gcc > "$TASK_RESULTS/received.sha256"
mkdir unpacked
tar -xzf xcpu-tool.tar.gz -C unpacked
tar -xJf xcpu-data.tar.xz -C unpacked
chmod 755 seedarc_append4_study.us2-gcc
cp /proc/cpuinfo "$TASK_RESULTS/cpuinfo.txt"
cp /proc/meminfo "$TASK_RESULTS/meminfo.txt"
( while sleep 5; do echo "$(date +%s) $(free -m | awk 'NR==2{print $3}') $(free -m | awk 'NR==3{print $3}')"; done ) > "$TASK_RESULTS/memory-trace.txt" &
ldd unpacked/xcpu-tool/seedarc_append4_study > "$TASK_RESULTS/ldd-india-build.txt"
ldd seedarc_append4_study.us2-gcc > "$TASK_RESULTS/ldd-us2-build.txt"
export LC_ALL=C.UTF-8
python3 -I unpacked/xcpu-tool/cross_cpu_check.py run unpacked/xcpu-tool/seedarc_append4_study unpacked/xdata w1 "$TASK_RESULTS/xcpu-vps-hk1.json"
python3 -I unpacked/xcpu-tool/cross_cpu_check.py run "$TASK_WORK/seedarc_append4_study.us2-gcc" unpacked/xdata w2 "$TASK_RESULTS/xcpu-vps-hk1-us2build.json"