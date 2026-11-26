#!/usr/bin/env bash
set -euo pipefail
cd "$TASK_WORK"
R="$TASK_RESULTS"; ENV=/home/sc/seedarc-env
python3 -I -c 'import zipfile,pathlib
z=zipfile.ZipFile("source.zip")
for e in z.infolist():
    p=pathlib.PurePosixPath(e.filename)
    assert not p.is_absolute() and ".." not in p.parts and "\\" not in e.filename
z.extractall("source")'
{ g++-14 --version | head -1; clang++-18 --version | head -1; cmake --version | head -1; ninja --version;
  git -C $ENV/vcpkg rev-parse HEAD; grep -E '^(Package|Version|Port-Version|Architecture):' $ENV/installed/vcpkg/status; } > "$R/toolchain.txt"
cp /proc/cpuinfo "$R/cpuinfo.txt"; cp /proc/meminfo "$R/meminfo.txt"
common=(-G Ninja -DCMAKE_PREFIX_PATH=$ENV/installed/x64-linux -DSEEDARC_BUILD_RESEARCH_TOOLS=ON -DBUILD_TESTING=ON)
cmake -S source -B release -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 "${common[@]}" > "$R/release-configure.log" 2>&1
cmake --build release --parallel 2 > "$R/release-build.log" 2>&1
echo "release warnings: $(grep -ciE 'warning' "$R/release-build.log" || true)"
ctest --test-dir release --output-on-failure --parallel 1 > "$R/release-ctest.log" 2>&1 || { tail -40 "$R/release-ctest.log"; exit 1; }
tail -3 "$R/release-ctest.log"
cmake -DTOOL=release/research/tools/seedarc_append4_study -DWORK="$TASK_WORK/bounded-cross" -DREPORT="$R/linux-cross-hashes.json" \
  -P source/research/tools/check_bounded_solid.cmake > "$R/bounded-cross.log" 2>&1
python3 -I -c 'import json,sys; a,b=(json.load(open(p)) for p in sys.argv[1:]); print("writer fixture hashes", "match India reference" if a==b else "DIFFER"); sys.exit(a!=b)' "$R/linux-cross-hashes.json" cross-reference.json
cp release/research/tools/seedarc_append4_study "$R/seedarc_append4_study.us2-gcc"
sha256sum "$R/seedarc_append4_study.us2-gcc" > "$R/tool.sha256"
rm -rf release
cmake -S source -B asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined" "${common[@]}" > "$R/asan-configure.log" 2>&1
cmake --build asan --parallel 2 > "$R/asan-build.log" 2>&1
echo "asan warnings: $(grep -ciE 'warning' "$R/asan-build.log" || true)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir asan --output-on-failure --parallel 1 > "$R/asan-ctest.log" 2>&1 || { tail -60 "$R/asan-ctest.log"; exit 1; }
tail -3 "$R/asan-ctest.log"
echo "sanitizer reports: $(grep -cE 'AddressSanitizer|runtime error:|LeakSanitizer' "$R/asan-ctest.log" || true)"