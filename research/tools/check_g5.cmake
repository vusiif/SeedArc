# SPDX-License-Identifier: GPL-3.0-or-later
# DEV-016: end-to-end checks of seedarc_g5_study on a small generated tree.
#   cmake -DTOOL=<seedarc_g5_study> -DWORK=<dir> -P check_g5.cmake
# Determinism (same search seed -> identical container bytes), byte-exact round trip for every
# method, the Seed field only for the seed method, strict rejection of damaged containers and of
# non-canonical plans, and refusal to overwrite.
cmake_minimum_required(VERSION 3.28)
file(REMOVE_RECURSE "${WORK}")
set(in "${WORK}/in")
file(MAKE_DIRECTORY "${in}/docs/a" "${in}/docs/b" "${in}/src" "${in}/cfg")

# Varied but deterministic content: text families per directory, sizes from 0 to > 64 KiB.
string(RANDOM LENGTH 4000 RANDOM_SEED 20261114 noise)
set(words "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda")
foreach(i RANGE 1 24)
    string(REPEAT "doc ${i} ${words} section ${i}\n" ${i} body)
    file(WRITE "${in}/docs/a/page${i}.txt" "${body}")
    file(WRITE "${in}/docs/b/note${i}.rst" "Title ${i}\n=====\n\n${body}${words}\n")
    string(REPEAT "def f${i}(x):\n    return x * ${i} + len('${words}')\n" 6 code)
    file(WRITE "${in}/src/mod${i}.py" "${code}")
    file(WRITE "${in}/cfg/pkg${i}.pc" "prefix=/usr\nName: pkg${i}\nVersion: 1.${i}\nLibs: -lpkg${i}\n")
endforeach()
string(REPEAT "${words} ${noise}\n" 40 big) # > 64 KiB: several blocks
file(WRITE "${in}/src/big.txt" "${big}")
file(WRITE "${in}/empty.txt" "")

function(run expect_code out_var)
    execute_process(COMMAND "${TOOL}" ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect_code)
        message(FATAL_ERROR "g5_study ${ARGN}: exit ${code}, expected ${expect_code}\n${out}\n${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

set(budget 3000000)
foreach(method random coord seed)
    run(0 first search ${method} "${in}" ${budget} 7 "${WORK}/${method}-1.sadx" 5)
    run(0 second search ${method} "${in}" ${budget} 7 "${WORK}/${method}-2.sadx" 5)
    file(SHA256 "${WORK}/${method}-1.sadx" h1)
    file(SHA256 "${WORK}/${method}-2.sadx" h2)
    if(NOT h1 STREQUAL h2)
        message(FATAL_ERROR "${method}: same search seed gave different containers")
    endif()
    string(JSON exact GET "${first}" byte_exact)
    string(JSON bytes GET "${first}" container_bytes)
    file(SIZE "${WORK}/${method}-1.sadx" size)
    if(NOT exact OR NOT bytes EQUAL size)
        message(FATAL_ERROR "${method}: not byte exact or size mismatch (${bytes} vs ${size})")
    endif()
    run(0 verified verify "${WORK}/${method}-1.sadx" "${in}")
    string(JSON seed_type TYPE "${verified}" seed)
    if(method STREQUAL "seed")
        if(NOT seed_type STREQUAL "NUMBER")
            message(FATAL_ERROR "seed method container carries no Seed (${seed_type})")
        endif()
    elseif(NOT seed_type STREQUAL "NULL")
        message(FATAL_ERROR "${method} container carries a Seed (${seed_type})")
    endif()
    # Refuses to overwrite.
    run(1 ignored search ${method} "${in}" ${budget} 7 "${WORK}/${method}-1.sadx" 5)
endforeach()

# The start plan (G-dict-r1) packs and verifies.
run(0 packed pack 0x40 "${in}" "${WORK}/start.sadx")
run(0 ignored verify "${WORK}/start.sadx" "${in}")
# A non-canonical plan (k = 1 with rule = 1) is refused.
run(2 ignored pack 0x41 "${in}" "${WORK}/bad-plan.sadx")
if(EXISTS "${WORK}/bad-plan.sadx")
    message(FATAL_ERROR "refused plan wrote a container")
endif()
# A budget too small for the start plan fails cleanly and writes nothing.
run(1 ignored search coord "${in}" 10 1 "${WORK}/tiny.sadx")
if(EXISTS "${WORK}/tiny.sadx")
    message(FATAL_ERROR "failed search wrote a container")
endif()

# Damaged and truncated containers are rejected, and every Seed round-trips (in the tool, since
# CMake cannot write arbitrary bytes portably).
run(0 selftest selftest "${in}")
string(JSON damaged GET "${selftest}" damaged_rejected)
string(JSON tried GET "${selftest}" damaged_tried)
if(NOT damaged EQUAL tried OR tried LESS 50)
    message(FATAL_ERROR "selftest: ${damaged} of ${tried} damaged containers rejected")
endif()
file(REMOVE_RECURSE "${WORK}")