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
{ clang++-18 --version | head -1; clang-scan-deps-18 --version | head -2 | tail -1; cmake --version | head -1; } > "$R/toolchain.txt"
cmake -S source -B asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/usr/bin/clang-scan-deps-18 \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined" \
  -DCMAKE_PREFIX_PATH=$ENV/installed/x64-linux -DSEEDARC_BUILD_RESEARCH_TOOLS=ON -DBUILD_TESTING=ON > "$R/asan-configure.log" 2>&1
cmake --build asan --parallel 2 > "$R/asan-build.log" 2>&1 || { tail -30 "$R/asan-build.log"; exit 1; }
echo "asan warnings: $(grep -ciE 'warning' "$R/asan-build.log" || true)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir asan --output-on-failure --parallel 1 > "$R/asan-ctest.log" 2>&1 || { tail -60 "$R/asan-ctest.log"; exit 1; }
tail -3 "$R/asan-ctest.log"
echo "sanitizer reports: $(grep -cE 'ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer' "$R/asan-ctest.log" || true)"