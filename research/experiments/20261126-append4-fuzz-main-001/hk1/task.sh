#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
R="$TASK_RESULTS"
sha256sum fuzz-pkg.tgz > "$R/received.sha256"
tar -xzf fuzz-pkg.tgz
sha256sum seedarc_fuzz_append4 >> "$R/received.sha256"
chmod 755 seedarc_fuzz_append4
ldd seedarc_fuzz_append4 > "$R/ldd.txt"
mkdir corpus art min
cp seeds/* corpus/
ls corpus | wc -l > "$R/seed-count.txt"
code=0
./seedarc_fuzz_append4 -max_total_time=36000 -rss_limit_mb=800 -malloc_limit_mb=600 -max_len=16384 -timeout=30 \
  -print_final_stats=1 -artifact_prefix="$TASK_WORK/art/" corpus > "$R/fuzz.log" 2>&1 || code=$?
echo "fuzz exit $code"
cp -r art "$R/"
./seedarc_fuzz_append4 -merge=1 -rss_limit_mb=800 -malloc_limit_mb=600 min corpus > "$R/merge.log" 2>&1 || echo "merge exit $?"
tar -czf "$R/corpus-min.tar.gz" -C min .
echo "corpus $(ls corpus | wc -l) minimized $(ls min | wc -l) artifacts $(ls art | wc -l)"
tail -2 "$R/fuzz.log"
