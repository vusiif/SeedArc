# DEV-013 shared-dictionary study — test split (generated from raw.json)

90 runs, all byte-exact; archives / containers identical across repetitions.
Sizes are complete artifacts (container: header + dictionary + group table + index + data). Times: medians.

## Size (bytes)

| config | test/config-cmake-pkg | test/many-small-docs | test/source-python | total |
| --- | ---: | ---: | ---: | ---: |
| P-lzma6 | 639,089 | 1,276,811 | 1,648,080 | 3,563,980 |
| P-zstd3 | 705,404 | 1,337,457 | 1,875,320 | 3,918,181 |
| C-dict-3-D64k-Tall | 586,407 | 978,730 | 1,776,202 | 3,341,339 |
| C-dict-9-D64k-Tall | 525,792 | 901,798 | 1,570,303 | 2,997,893 |
| C-nodict-3 | 685,195 | 1,245,302 | 1,853,588 | 3,784,085 |
| C-nodict-9 | 642,612 | 1,198,465 | 1,702,927 | 3,544,004 |

## Time and memory (median ms; peak MiB)

| dataset | config | compress wall | compress CPU | train wall | encode wall | extract wall | single-file median / p90 | peak c / x MiB | dict B | index B |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: |
| test/config-cmake-pkg | P-lzma6 | 470.3 | 468.8 | — | — | 410.6 | 1.89 / 2.28 | 8.6 / 7.3 | 0 | 44,675 |
| test/config-cmake-pkg | P-zstd3 | 99.1 | 93.8 | — | — | 403.0 | 1.96 / 2.33 | 8.3 / 7.4 | 0 | 44,675 |
| test/config-cmake-pkg | C-dict-3-D64k-Tall | 254.4 | 250.0 | 159.4 | 30.7 | 26.4 | 0.75 / 0.92 | 20.8 / 10.3 | 65,536 | 24,194 |
| test/config-cmake-pkg | C-dict-9-D64k-Tall | 253.2 | 250.0 | 135.6 | 72.4 | 25.6 | 0.58 / 0.77 | 20.8 / 10.3 | 65,536 | 24,194 |
| test/config-cmake-pkg | C-nodict-3 | 102.6 | 109.4 | 0.0 | 32.2 | 25.5 | 0.89 / 1.08 | 9.9 / 10.3 | 0 | 24,194 |
| test/config-cmake-pkg | C-nodict-9 | 114.8 | 109.4 | 0.0 | 57.9 | 25.2 | 0.68 / 0.92 | 9.8 / 10.2 | 0 | 24,194 |
| test/many-small-docs | P-lzma6 | 940.7 | 937.5 | — | — | 1,860.5 | 7.87 / 9.91 | 10.0 / 7.9 | 0 | 226,118 |
| test/many-small-docs | P-zstd3 | 321.2 | 328.1 | — | — | 1,697.1 | 7.50 / 8.77 | 10.0 / 8.1 | 0 | 226,118 |
| test/many-small-docs | C-dict-3-D64k-Tall | 468.7 | 468.8 | 166.7 | 43.5 | 33.4 | 1.13 / 1.30 | 22.6 / 12.9 | 65,536 | 132,406 |
| test/many-small-docs | C-dict-9-D64k-Tall | 568.0 | 578.1 | 208.6 | 107.5 | 34.3 | 1.07 / 1.41 | 22.6 / 12.7 | 65,536 | 132,406 |
| test/many-small-docs | C-nodict-3 | 323.0 | 328.1 | 0.0 | 44.9 | 35.0 | 1.33 / 1.64 | 11.5 / 13.0 | 0 | 132,406 |
| test/many-small-docs | C-nodict-9 | 329.4 | 328.1 | 0.0 | 81.1 | 34.6 | 1.13 / 1.35 | 11.5 / 12.9 | 0 | 132,406 |
| test/source-python | P-lzma6 | 1,193.9 | 1,187.5 | — | — | 638.3 | 2.40 / 3.09 | 8.6 / 7.3 | 0 | 39,742 |
| test/source-python | P-zstd3 | 183.3 | 171.9 | — | — | 588.2 | 2.50 / 3.13 | 8.3 / 7.4 | 0 | 39,742 |
| test/source-python | C-dict-3-D64k-Tall | 481.1 | 484.4 | 317.1 | 88.5 | 73.9 | 1.24 / 1.53 | 28.6 / 19.7 | 65,536 | 17,737 |
| test/source-python | C-dict-9-D64k-Tall | 677.4 | 671.9 | 407.2 | 221.1 | 71.5 | 1.28 / 1.75 | 28.6 / 19.5 | 65,536 | 17,737 |
| test/source-python | C-nodict-3 | 146.0 | 140.6 | 0.0 | 82.0 | 72.1 | 1.40 / 1.85 | 15.9 / 19.6 | 0 | 17,737 |
| test/source-python | C-nodict-9 | 258.0 | 265.6 | 0.0 | 182.3 | 70.0 | 1.42 / 1.84 | 16.1 / 19.5 | 0 | 17,737 |

## Per-file stored bytes vs same-level no-dictionary container

| dataset | config | files +/0/− | mean | median | p10 | p90 | worst |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| test/config-cmake-pkg | C-dict-3-D64k-Tall | 333/2/1 | 489.1 | 322 | 43 | 1,110 | cmake-share/cmake-4.4/Modules/CheckTypeSize.c.in (-3) |
| test/config-cmake-pkg | C-dict-9-D64k-Tall | 334/2/0 | 542.7 | 362 | 86 | 1,316 | — |
| test/many-small-docs | C-dict-3-D64k-Tall | 1621/0/0 | 204.9 | 140 | 20 | 374 | — |
| test/many-small-docs | C-dict-9-D64k-Tall | 1620/1/0 | 223.4 | 146 | 16 | 396 | — |
| test/source-python | C-dict-3-D64k-Tall | 327/2/8 | 424.1 | 339 | 87 | 890 | python-lib/tkinter/constants.py (-73), python-lib/curses/has_key.py (-67) |
| test/source-python | C-dict-9-D64k-Tall | 335/2/0 | 588.0 | 441 | 110 | 1,235 | — |
