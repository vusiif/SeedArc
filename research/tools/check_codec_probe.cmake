# SPDX-License-Identifier: GPL-3.0-or-later
# codec-probe (research/ideas/codec-capabilities-r0.md): every variant round-trips (the tool fails
# otherwise), zstd output does not depend on the worker count, and bad variant names are refused.
cmake_minimum_required(VERSION 3.28)
file(MAKE_DIRECTORY "${WORK}")
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef token)
set(root "${WORK}/run-${token}")
if(EXISTS "${root}")
    message(FATAL_ERROR "Refuse fixture reuse")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/bounded_fixture.py" "${root}"
    RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "portable fixture creation failed")
endif()
set(variants "zstd-3,zstd-22,zstd-9-ldm,zstd-9-mt1,zstd-9-mt2,lzma2-0,lzma2-6e,lzma2-6-bcj,lzma2-9e-bcj")
foreach(command codec-probe codec-probe-classes)
    execute_process(COMMAND "${TOOL}" ${command} "${root}/all" "${variants}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
        message(FATAL_ERROR "${command} failed: ${err}")
    endif()
    string(JSON groups LENGTH "${out}" groups)
    string(JSON count LENGTH "${out}" groups 0 variants)
    if(groups LESS 1 OR NOT count EQUAL 9)
        message(FATAL_ERROR "${command} output incomplete: ${out}")
    endif()
endforeach()
# Same worker count independence on the single-frame run: mt1 and mt2 give the same frame.
execute_process(COMMAND "${TOOL}" codec-probe "${root}/all" "zstd-9-mt1,zstd-9-mt2" OUTPUT_VARIABLE out)
string(JSON a GET "${out}" groups 0 variants 0 frame_crc32c)
string(JSON b GET "${out}" groups 0 variants 1 frame_crc32c)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "zstd multithreaded output depends on the worker count")
endif()
foreach(bad "zstd-0" "zstd-23" "zstd-9-mt0" "zstd-9-ldm-ldm" "zstd-09" "lzma2-10" "lzma2-6-ldm"
        "lzma2-6ee" "brotli-5" "zstd-9," "lzma2-6-bcj-bcj")
    execute_process(COMMAND "${TOOL}" codec-probe "${root}/all" "${bad}" RESULT_VARIABLE code
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 2)
        message(FATAL_ERROR "codec-probe accepted ${bad} (exit ${code})")
    endif()
endforeach()
# Framed mode cuts frames exactly as recipe-pack does: on the 21 MiB fixture the 16 MiB step
# gives two frames, and the summed frame bytes equal the archive's data part for both codecs
# and for the LZMA2 extreme recipe (ADR-0007).
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/recipe_fixture.py" big "${root}/big"
    RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "recipe fixture creation failed")
endif()
execute_process(COMMAND "${TOOL}" codec-probe-framed "${root}/big" "zstd-9,lzma2-9,lzma2-9e" 16MiB
    RESULT_VARIABLE code OUTPUT_VARIABLE framed ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
    message(FATAL_ERROR "codec-probe-framed failed: ${err}")
endif()
string(JSON frames GET "${framed}" groups 0 frames)
if(NOT frames EQUAL 2)
    message(FATAL_ERROR "framed probe did not split 21 MiB into two frames: ${framed}")
endif()
foreach(pair "0;zstd-9" "1;lzma2-9" "2;lzma2-9e")
    list(GET pair 0 index)
    list(GET pair 1 codec)
    execute_process(COMMAND "${TOOL}" recipe-pack seed "solid/${codec}/g1-r0/frame-16MiB" "${root}/big"
        "${root}/big-${codec}.sadx" RESULT_VARIABLE code OUTPUT_VARIABLE packed ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "recipe-pack ${codec} failed: ${err}")
    endif()
    string(JSON data GET "${packed}" parts data)
    string(JSON probe GET "${framed}" groups 0 variants ${index} compressed_bytes)
    if(NOT data EQUAL probe)
        message(FATAL_ERROR "${codec}: framed probe ${probe} bytes, recipe-pack data ${data} bytes")
    endif()
endforeach()
foreach(bad whole 0MiB 48MiB 32mib 1024MiB)
    execute_process(COMMAND "${TOOL}" codec-probe-framed "${root}/all" "zstd-9" "${bad}" RESULT_VARIABLE code
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 2)
        message(FATAL_ERROR "codec-probe-framed accepted frame limit ${bad} (exit ${code})")
    endif()
endforeach()
# dup-probe (DEV-036): big = a b c (7 MiB each, distinct). Add a2 = d = copy of a and two empty
# files. At 16 MiB: baseline frames [a a2] [b c] [d e0 e1]; a2 duplicates a in the same frame,
# d in another frame; empty files never count. Dedup keeps a b c e0 e1: frames [a b] [c e0 e1];
# dedup_same_cut keeps the baseline frames without the copies: [a] [b c] [e0 e1].
file(COPY_FILE "${root}/big/a" "${root}/big/a2")
file(COPY_FILE "${root}/big/a" "${root}/big/d")
file(TOUCH "${root}/big/e0" "${root}/big/e1")
execute_process(COMMAND "${TOOL}" dup-probe "${root}/big" zstd-3 16MiB
    RESULT_VARIABLE code OUTPUT_VARIABLE dup ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
    message(FATAL_ERROR "dup-probe failed: ${err}")
endif()
foreach(check "duplicates files;2" "duplicates bytes;14680064" "duplicates cross_frame_files;1"
        "duplicates cross_frame_bytes;7340032" "duplicates distinct_contents_with_copies;1"
        "baseline files;7" "baseline frames;3" "dedup files;5" "dedup frames;2"
        "dedup raw_bytes;22020096" "files;7" "dedup_same_cut files;5" "dedup_same_cut frames;3"
        "dedup_same_cut raw_bytes;22020096")
    list(GET check 0 where)
    list(GET check 1 want)
    string(REPLACE " " ";" where "${where}")
    string(JSON got GET "${dup}" ${where})
    if(NOT got EQUAL want)
        message(FATAL_ERROR "dup-probe ${where}: ${got}, expected ${want}\n${dup}")
    endif()
endforeach()
string(JSON base_bytes GET "${dup}" baseline compressed_bytes)
string(JSON dedup_bytes GET "${dup}" dedup compressed_bytes)
if(NOT dedup_bytes LESS base_bytes)
    message(FATAL_ERROR "dedup did not shrink the frames: ${dup}")
endif()
foreach(bad "zstd-3;48MiB" "brotli-5;16MiB" "zstd-3;16mib")
    list(GET bad 0 variant)
    list(GET bad 1 limit)
    execute_process(COMMAND "${TOOL}" dup-probe "${root}/big" "${variant}" "${limit}" RESULT_VARIABLE code
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 2)
        message(FATAL_ERROR "dup-probe accepted ${variant} ${limit} (exit ${code})")
    endif()
endforeach()
# cut-probe (DEV-037): on the same tree greedy equals the dup-probe baseline, and shift-1 (first
# frame at most 2 MiB) puts a alone in the first frame.
execute_process(COMMAND "${TOOL}" cut-probe "${root}/big" zstd-3 16MiB
    RESULT_VARIABLE code OUTPUT_VARIABLE cut ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
    message(FATAL_ERROR "cut-probe failed: ${err}")
endif()
string(JSON greedy_name GET "${cut}" rules 0 rule)
string(JSON greedy_bytes GET "${cut}" rules 0 compressed_bytes)
string(JSON shift_frames GET "${cut}" rules 1 frames)
string(JSON shift_first GET "${cut}" rules 1 frame_raw 0)
if(NOT greedy_name STREQUAL "greedy" OR NOT greedy_bytes EQUAL base_bytes OR NOT shift_frames EQUAL 3
        OR NOT shift_first EQUAL 7340032)
    message(FATAL_ERROR "cut-probe greedy / shift-1 mismatch (baseline ${base_bytes}): ${cut}")
endif()
# Two directories of 40 files, 256 KiB + 4 bytes each; files in p/ share one random block, files
# in q/ another. Greedy at 16 MiB closes after 63 files, inside q/. With a 50% window the p|q
# boundary (40 files, 10 MiB) is a candidate: path-w50 (0 common levels), sim-w50 (no shared
# windows) and balanced (target ceil(total / 2) = exactly p/) all cut there and compress
# better. With a 25% window it is not a candidate and both rules keep the greedy frames.
execute_process(COMMAND "${Python3_EXECUTABLE}" -I -c [[
import random, sys
from pathlib import Path
for d, seed in (('p', 1), ('q', 2)):
    block = random.Random(seed).randbytes(262144)
    (Path(sys.argv[1]) / d).mkdir(parents=True)
    for i in range(40):
        (Path(sys.argv[1]) / d / f'{i:03}').write_bytes(block + i.to_bytes(4, 'little'))
]] "${root}/cut" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cut fixture creation failed")
endif()
foreach(jobs 1 3)
    execute_process(COMMAND "${TOOL}" --jobs ${jobs} cut-probe "${root}/cut" zstd-3 16MiB
        RESULT_VARIABLE code OUTPUT_VARIABLE cut ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
        message(FATAL_ERROR "cut-probe --jobs ${jobs} failed: ${err}")
    endif()
    string(JSON rules_${jobs} GET "${cut}" rules)
endforeach()
if(NOT rules_1 STREQUAL rules_3)
    message(FATAL_ERROR "cut-probe results depend on the worker count")
endif()
string(JSON greedy_first GET "${cut}" rules 0 frame_raw 0)
string(JSON greedy_chain GET "${cut}" rules 0 frame_crc_chain)
string(JSON greedy_bytes GET "${cut}" rules 0 compressed_bytes)
if(NOT greedy_first EQUAL 16515324)
    message(FATAL_ERROR "cut-probe greedy first frame ${greedy_first}: ${cut}")
endif()
foreach(index 8 10 12)
    string(JSON name GET "${cut}" rules ${index} rule)
    string(JSON first GET "${cut}" rules ${index} frame_raw 0)
    string(JSON bytes GET "${cut}" rules ${index} compressed_bytes)
    if(NOT first EQUAL 10485920 OR NOT bytes LESS greedy_bytes)
        message(FATAL_ERROR "cut-probe ${name}: first frame ${first}, ${bytes} bytes (greedy ${greedy_bytes})")
    endif()
endforeach()
# path-w25 sees equal levels everywhere and keeps the latest (greedy) boundary. sim-w25 may pick
# another q|q boundary (the 4-byte tags make sketches differ slightly) but never one before the
# window: the first frame keeps at least 12 MiB.
string(JSON chain GET "${cut}" rules 9 frame_crc_chain)
string(JSON sim_first GET "${cut}" rules 11 frame_raw 0)
if(NOT chain EQUAL greedy_chain OR sim_first LESS 12582912)
    message(FATAL_ERROR "cut-probe 25% window rules: ${cut}")
endif()
foreach(bad "zstd-3;whole" "brotli-5;16MiB" "zstd-3;32mib")
    list(GET bad 0 variant)
    list(GET bad 1 limit)
    execute_process(COMMAND "${TOOL}" cut-probe "${root}/cut" "${variant}" "${limit}" RESULT_VARIABLE code
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 2)
        message(FATAL_ERROR "cut-probe accepted ${variant} ${limit} (exit ${code})")
    endif()
endforeach()
# memory-estimate (DEV-039): a worker holds the frame, so the estimate is above the frame size;
# it grows with the frame and the level, zstd-19 at 2^27 includes long-distance matching (the
# case where zstd's own estimate function divides by zero), and bad recipes are refused.
set(previous 0)
foreach(recipe "solid/zstd-3/g1-r0/frame-32MiB" "solid/zstd-19/g1-r0/frame-32MiB" "solid/zstd-19/g1-r0/frame-128MiB"
        "solid/lzma2-9e/g1-r0/frame-128MiB")
    execute_process(COMMAND "${TOOL}" memory-estimate "${recipe}" RESULT_VARIABLE code OUTPUT_VARIABLE est
        ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR err MATCHES "AddressSanitizer|runtime error:|LeakSanitizer")
        message(FATAL_ERROR "memory-estimate ${recipe} failed (${code}): ${err}")
    endif()
    string(JSON frame GET "${est}" frame_bytes)
    string(JSON worker GET "${est}" per_worker_bytes)
    if(NOT worker GREATER frame OR NOT worker GREATER previous)
        message(FATAL_ERROR "memory-estimate ${recipe}: ${est} (previous ${previous})")
    endif()
    set(previous ${worker})
endforeach()
foreach(bad "solid/zstd-20/g1-r0/frame-32MiB" "solid/zstd-9/g1-r0/frame-48MiB")
    execute_process(COMMAND "${TOOL}" memory-estimate "${bad}" RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
    if(NOT code EQUAL 2)
        message(FATAL_ERROR "memory-estimate accepted ${bad} (exit ${code})")
    endif()
endforeach()
message(STATUS "codec-probe: round trips, worker independence, recipe-pack framing and refusals passed")
cmake_path(IS_PREFIX WORK "${root}" NORMALIZE owned)
if(NOT owned OR IS_SYMLINK "${root}")
    message(FATAL_ERROR "unexpected cleanup path")
endif()
file(REMOVE_RECURSE "${root}")
