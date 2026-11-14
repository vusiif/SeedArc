# DEV-013 shared-dictionary study — dev split (generated from raw.json)

252 runs, all byte-exact; archives / containers identical across repetitions.
Sizes are complete artifacts (container: header + dictionary + group table + index + data). Times: medians.

## Size (bytes)

| config | dev/config-cmake-pkg | dev/many-small-docs | dev/source-python | total |
| --- | ---: | ---: | ---: | ---: |
| P-lzma6 | 1,776,038 | 470,126 | 1,486,193 | 3,732,357 |
| P-zstd3 | 1,879,986 | 473,647 | 1,708,952 | 4,062,585 |
| C-dict-3-D112k-T1 | 1,591,932 | 356,369 | 1,574,551 | 3,522,852 |
| C-dict-3-D112k-T4 | 1,584,444 | 356,369 | 1,567,838 | 3,508,651 |
| C-dict-3-D112k-Tall | 1,584,444 | 356,369 | 1,564,255 | 3,505,068 |
| C-dict-3-D16k-T1 | 1,596,058 | 319,043 | 1,602,500 | 3,517,601 |
| C-dict-3-D16k-T4 | 1,578,393 | 319,043 | 1,587,685 | 3,485,121 |
| C-dict-3-D16k-Tall | 1,578,393 | 319,043 | 1,582,604 | 3,480,040 |
| C-dict-3-D256k-T1 | 1,639,335 | 456,448 | 1,686,194 | 3,781,977 |
| C-dict-3-D256k-T4 | 1,671,690 | 456,448 | 1,743,886 | 3,872,024 |
| C-dict-3-D256k-Tall | 1,671,690 | 456,448 | 1,637,733 | 3,765,871 |
| C-dict-3-D64k-T1 | 1,580,199 | 328,195 | 1,548,452 | 3,456,846 |
| C-dict-3-D64k-T4 | 1,566,788 | 328,195 | 1,553,654 | 3,448,637 |
| C-dict-3-D64k-Tall | 1,566,788 | 328,195 | 1,547,410 | 3,442,393 |
| C-dict-9-D112k-T1 | 1,474,754 | 337,402 | 1,391,600 | 3,203,756 |
| C-dict-9-D112k-T4 | 1,464,512 | 337,402 | 1,390,361 | 3,192,275 |
| C-dict-9-D112k-Tall | 1,464,512 | 337,402 | 1,388,483 | 3,190,397 |
| C-dict-9-D16k-T1 | 1,498,199 | 303,052 | 1,440,759 | 3,242,010 |
| C-dict-9-D16k-T4 | 1,481,618 | 303,052 | 1,424,136 | 3,208,806 |
| C-dict-9-D16k-Tall | 1,481,618 | 303,052 | 1,416,042 | 3,200,712 |
| C-dict-9-D256k-T1 | 1,529,022 | 438,141 | 1,466,324 | 3,433,487 |
| C-dict-9-D256k-T4 | 1,526,955 | 438,141 | 1,504,403 | 3,469,499 |
| C-dict-9-D256k-Tall | 1,526,955 | 438,141 | 1,459,149 | 3,424,245 |
| C-dict-9-D64k-T1 | 1,467,028 | 309,900 | 1,377,981 | 3,154,909 |
| C-dict-9-D64k-T4 | 1,455,012 | 309,900 | 1,383,547 | 3,148,459 |
| C-dict-9-D64k-Tall | 1,455,012 | 309,900 | 1,376,774 | 3,141,686 |
| C-nodict-3 | 1,796,340 | 430,607 | 1,687,118 | 3,914,065 |
| C-nodict-9 | 1,724,603 | 422,143 | 1,557,146 | 3,703,892 |

## Time and memory (median ms; peak MiB)

| dataset | config | compress wall | compress CPU | train wall | encode wall | extract wall | single-file median / p90 | peak c / x MiB | dict B | index B |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: |
| dev/config-cmake-pkg | P-lzma6 | 1,410.8 | 1,390.6 | — | — | 1,719.2 | 8.31 / 9.72 | 9.8 / 7.8 | 0 | 203,214 |
| dev/config-cmake-pkg | P-zstd3 | 312.1 | 312.5 | — | — | 1,910.0 | 7.72 / 9.71 | 10.4 / 7.9 | 0 | 203,214 |
| dev/config-cmake-pkg | C-dict-3-D112k-T1 | 342.9 | 343.8 | 72.7 | 58.3 | 51.1 | 1.83 / 2.55 | 22.4 / 16.4 | 114,688 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D112k-T4 | 514.6 | 515.6 | 223.5 | 64.1 | 52.9 | 1.48 / 1.68 | 25.1 / 16.3 | 97,247 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D112k-Tall | 520.7 | 531.2 | 220.2 | 62.4 | 50.1 | 1.34 / 1.53 | 25.1 / 16.4 | 97,247 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D16k-T1 | 373.6 | 375.0 | 73.1 | 62.7 | 50.9 | 1.28 / 1.42 | 21.9 / 16.2 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D16k-T4 | 553.3 | 546.9 | 246.7 | 67.8 | 52.4 | 1.52 / 1.69 | 24.6 / 16.2 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D16k-Tall | 515.8 | 500.0 | 229.4 | 64.1 | 51.6 | 1.52 / 1.86 | 24.6 / 16.2 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D256k-T1 | 413.7 | 406.2 | 130.8 | 66.2 | 53.5 | 1.80 / 2.41 | 23.7 / 16.7 | 262,144 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D256k-T4 | 683.1 | 656.2 | 343.3 | 74.9 | 51.3 | 1.62 / 1.85 | 26.6 / 16.6 | 221,633 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D256k-Tall | 470.0 | 468.8 | 213.6 | 69.0 | 49.8 | 1.47 / 1.68 | 26.8 / 16.6 | 221,633 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D64k-T1 | 381.1 | 390.6 | 75.9 | 63.5 | 50.3 | 1.58 / 2.05 | 22.1 / 16.3 | 65,536 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D64k-T4 | 694.9 | 656.2 | 376.7 | 76.9 | 54.5 | 1.40 / 1.57 | 25.0 / 16.2 | 55,808 | 118,201 |
| dev/config-cmake-pkg | C-dict-3-D64k-Tall | 494.4 | 484.4 | 221.5 | 63.5 | 49.7 | 1.56 / 1.74 | 24.8 / 16.2 | 55,808 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D112k-T1 | 456.7 | 453.1 | 101.3 | 130.4 | 50.9 | 1.18 / 1.43 | 22.3 / 16.3 | 114,688 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D112k-T4 | 554.1 | 546.9 | 185.9 | 127.3 | 48.8 | 1.37 / 1.60 | 25.1 / 16.2 | 97,247 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D112k-Tall | 627.9 | 625.0 | 257.6 | 138.8 | 52.1 | 1.44 / 1.77 | 25.1 / 16.2 | 97,247 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D16k-T1 | 418.1 | 421.9 | 77.6 | 116.0 | 51.0 | 1.14 / 1.26 | 21.9 / 16.1 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D16k-T4 | 505.4 | 500.0 | 179.2 | 125.3 | 50.4 | 1.20 / 1.50 | 24.6 / 16.1 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D16k-Tall | 544.7 | 546.9 | 225.0 | 121.0 | 51.7 | 1.53 / 1.84 | 24.6 / 16.1 | 16,384 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D256k-T1 | 495.6 | 484.4 | 135.0 | 132.0 | 49.7 | 1.32 / 1.53 | 24.0 / 16.6 | 262,144 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D256k-T4 | 591.3 | 578.1 | 220.9 | 158.8 | 50.4 | 1.62 / 2.19 | 26.7 / 16.5 | 221,633 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D256k-Tall | 643.0 | 640.6 | 253.2 | 165.0 | 51.4 | 1.31 / 1.51 | 26.7 / 16.5 | 221,633 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D64k-T1 | 511.9 | 515.6 | 107.7 | 144.2 | 50.2 | 1.30 / 1.78 | 22.1 / 16.2 | 65,536 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D64k-T4 | 545.7 | 546.9 | 188.2 | 130.6 | 49.4 | 1.13 / 1.49 | 24.8 / 16.2 | 55,808 | 118,201 |
| dev/config-cmake-pkg | C-dict-9-D64k-Tall | 555.5 | 562.5 | 215.1 | 131.2 | 50.3 | 1.37 / 1.62 | 24.8 / 16.2 | 55,808 | 118,201 |
| dev/config-cmake-pkg | C-nodict-3 | 302.5 | 312.5 | 0.0 | 62.3 | 51.0 | 1.36 / 1.55 | 14.3 / 16.3 | 0 | 118,201 |
| dev/config-cmake-pkg | C-nodict-9 | 314.7 | 312.5 | 0.0 | 110.6 | 50.0 | 1.46 / 1.74 | 15.2 / 16.2 | 0 | 118,201 |
| dev/many-small-docs | P-lzma6 | 510.1 | 515.6 | — | — | 779.5 | 3.45 / 4.12 | 8.8 / 7.3 | 0 | 105,613 |
| dev/many-small-docs | P-zstd3 | 155.5 | 156.2 | — | — | 758.3 | 3.92 / 4.63 | 8.6 / 7.4 | 0 | 105,613 |
| dev/many-small-docs | C-dict-3-D112k-T1 | 213.3 | 218.8 | 89.1 | 11.5 | 11.8 | 0.59 / 0.70 | 18.7 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-3-D112k-T4 | 225.3 | 234.4 | 95.3 | 12.0 | 11.4 | 0.61 / 0.71 | 18.7 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-3-D112k-Tall | 257.4 | 250.0 | 123.0 | 12.2 | 11.5 | 0.52 / 0.64 | 18.6 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-3-D16k-T1 | 186.9 | 187.5 | 64.2 | 11.6 | 10.9 | 0.47 / 0.57 | 18.1 / 7.6 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-3-D16k-T4 | 231.3 | 234.4 | 78.3 | 12.7 | 10.8 | 0.46 / 0.55 | 18.3 / 7.6 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-3-D16k-Tall | 212.6 | 203.1 | 79.3 | 12.2 | 11.3 | 0.49 / 0.68 | 17.9 / 7.6 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-3-D256k-T1 | 279.9 | 281.2 | 153.7 | 12.3 | 10.8 | 0.59 / 0.69 | 19.9 / 8.2 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-3-D256k-T4 | 251.9 | 250.0 | 125.7 | 11.6 | 10.7 | 0.71 / 0.84 | 19.8 / 8.2 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-3-D256k-Tall | 298.5 | 296.9 | 178.2 | 13.1 | 11.7 | 0.77 / 0.89 | 19.8 / 8.2 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-3-D64k-T1 | 216.8 | 218.8 | 79.2 | 11.5 | 10.9 | 0.47 / 0.59 | 18.7 / 7.7 | 65,536 | 61,891 |
| dev/many-small-docs | C-dict-3-D64k-T4 | 198.2 | 203.1 | 66.3 | 12.1 | 10.8 | 0.46 / 0.61 | 18.4 / 7.7 | 65,536 | 61,891 |
| dev/many-small-docs | C-dict-3-D64k-Tall | 189.1 | 187.5 | 62.4 | 11.7 | 10.5 | 0.45 / 0.55 | 18.6 / 7.7 | 65,536 | 61,891 |
| dev/many-small-docs | C-dict-9-D112k-T1 | 232.5 | 234.4 | 86.6 | 26.3 | 11.3 | 0.57 / 0.68 | 18.7 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-9-D112k-T4 | 221.6 | 218.8 | 86.1 | 25.5 | 10.8 | 0.52 / 0.67 | 18.7 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-9-D112k-Tall | 231.2 | 234.4 | 86.2 | 26.3 | 10.3 | 0.53 / 0.69 | 18.6 / 7.8 | 114,688 | 61,891 |
| dev/many-small-docs | C-dict-9-D16k-T1 | 236.6 | 234.4 | 83.2 | 26.1 | 11.6 | 0.47 / 0.60 | 18.3 / 7.5 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-9-D16k-T4 | 208.1 | 203.1 | 67.5 | 28.6 | 10.8 | 0.50 / 0.62 | 18.0 / 7.6 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-9-D16k-Tall | 211.2 | 218.8 | 69.5 | 26.5 | 10.7 | 0.44 / 0.58 | 17.9 / 7.5 | 16,384 | 61,891 |
| dev/many-small-docs | C-dict-9-D256k-T1 | 251.4 | 250.0 | 125.6 | 25.5 | 11.1 | 0.65 / 0.75 | 19.9 / 8.1 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-9-D256k-T4 | 285.1 | 281.2 | 155.0 | 24.4 | 11.0 | 0.75 / 0.89 | 19.8 / 8.1 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-9-D256k-Tall | 345.3 | 343.8 | 205.9 | 24.6 | 11.0 | 0.66 / 0.80 | 19.8 / 8.1 | 262,144 | 61,891 |
| dev/many-small-docs | C-dict-9-D64k-T1 | 202.3 | 203.1 | 63.3 | 28.1 | 10.6 | 0.52 / 0.73 | 18.2 / 7.7 | 65,536 | 61,891 |
| dev/many-small-docs | C-dict-9-D64k-T4 | 209.4 | 218.8 | 70.6 | 29.6 | 10.7 | 0.49 / 0.55 | 18.2 / 7.6 | 65,536 | 61,891 |
| dev/many-small-docs | C-dict-9-D64k-Tall | 211.2 | 203.1 | 75.4 | 27.7 | 11.6 | 0.49 / 0.57 | 18.2 / 7.6 | 65,536 | 61,891 |
| dev/many-small-docs | C-nodict-3 | 129.4 | 125.0 | 0.0 | 12.9 | 11.1 | 0.40 / 0.53 | 7.5 / 7.6 | 0 | 61,891 |
| dev/many-small-docs | C-nodict-9 | 151.6 | 156.2 | 0.0 | 22.7 | 12.7 | 0.60 / 0.71 | 7.5 / 7.6 | 0 | 61,891 |
| dev/source-python | P-lzma6 | 1,157.5 | 1,156.2 | — | — | 607.2 | 2.34 / 2.89 | 8.6 / 7.2 | 0 | 41,337 |
| dev/source-python | P-zstd3 | 170.6 | 171.9 | — | — | 557.1 | 2.48 / 3.40 | 8.8 / 7.3 | 0 | 41,337 |
| dev/source-python | C-dict-3-D112k-T1 | 268.1 | 265.6 | 101.3 | 92.0 | 73.4 | 2.09 / 2.61 | 23.8 / 19.0 | 114,688 | 19,218 |
| dev/source-python | C-dict-3-D112k-T4 | 454.5 | 453.1 | 309.1 | 84.1 | 68.6 | 1.37 / 1.64 | 27.0 / 19.0 | 114,688 | 19,218 |
| dev/source-python | C-dict-3-D112k-Tall | 430.2 | 421.9 | 292.3 | 83.5 | 68.6 | 1.26 / 1.51 | 28.2 / 19.0 | 114,688 | 19,218 |
| dev/source-python | C-dict-3-D16k-T1 | 243.9 | 250.0 | 86.5 | 83.9 | 66.9 | 1.32 / 1.49 | 23.3 / 18.8 | 16,384 | 19,218 |
| dev/source-python | C-dict-3-D16k-T4 | 381.9 | 375.0 | 235.6 | 85.2 | 66.7 | 1.99 / 2.53 | 26.3 / 18.8 | 16,384 | 19,218 |
| dev/source-python | C-dict-3-D16k-Tall | 596.9 | 593.8 | 409.5 | 91.2 | 69.4 | 1.49 / 1.78 | 27.2 / 18.8 | 16,384 | 19,218 |
| dev/source-python | C-dict-3-D256k-T1 | 265.3 | 265.6 | 124.4 | 82.8 | 71.3 | 1.98 / 2.36 | 25.1 / 19.1 | 144,539 | 19,218 |
| dev/source-python | C-dict-3-D256k-T4 | 425.8 | 421.9 | 272.9 | 86.3 | 72.3 | 2.46 / 2.79 | 28.5 / 19.3 | 217,724 | 19,218 |
| dev/source-python | C-dict-3-D256k-Tall | 644.1 | 640.6 | 462.0 | 98.9 | 75.5 | 1.56 / 2.05 | 30.3 / 19.3 | 262,144 | 19,218 |
| dev/source-python | C-dict-3-D64k-T1 | 243.0 | 234.4 | 90.2 | 92.7 | 68.4 | 1.28 / 1.58 | 23.5 / 18.9 | 65,536 | 19,218 |
| dev/source-python | C-dict-3-D64k-T4 | 428.0 | 421.9 | 282.8 | 91.4 | 67.0 | 1.23 / 1.54 | 26.6 / 18.9 | 65,536 | 19,218 |
| dev/source-python | C-dict-3-D64k-Tall | 484.4 | 484.4 | 325.3 | 91.5 | 73.5 | 1.57 / 1.88 | 27.5 / 18.9 | 65,536 | 19,218 |
| dev/source-python | C-dict-9-D112k-T1 | 367.0 | 375.0 | 110.3 | 199.3 | 70.4 | 1.24 / 1.68 | 23.8 / 18.8 | 114,688 | 19,218 |
| dev/source-python | C-dict-9-D112k-T4 | 504.6 | 500.0 | 251.4 | 191.8 | 67.7 | 1.20 / 1.49 | 27.0 / 18.8 | 114,688 | 19,218 |
| dev/source-python | C-dict-9-D112k-Tall | 494.7 | 484.4 | 250.9 | 195.7 | 66.9 | 1.08 / 1.25 | 28.2 / 18.8 | 114,688 | 19,218 |
| dev/source-python | C-dict-9-D16k-T1 | 316.6 | 312.5 | 74.9 | 178.8 | 69.7 | 1.10 / 1.36 | 23.2 / 18.7 | 16,384 | 19,218 |
| dev/source-python | C-dict-9-D16k-T4 | 475.6 | 484.4 | 229.5 | 171.6 | 67.6 | 1.14 / 1.44 | 26.3 / 18.7 | 16,384 | 19,218 |
| dev/source-python | C-dict-9-D16k-Tall | 605.8 | 609.4 | 362.8 | 182.4 | 67.2 | 1.15 / 1.43 | 27.2 / 18.6 | 16,384 | 19,218 |
| dev/source-python | C-dict-9-D256k-T1 | 407.7 | 406.2 | 123.5 | 225.9 | 67.8 | 1.27 / 1.57 | 25.1 / 18.9 | 144,539 | 19,218 |
| dev/source-python | C-dict-9-D256k-T4 | 550.3 | 546.9 | 247.7 | 249.6 | 71.1 | 1.70 / 2.24 | 28.5 / 19.1 | 217,724 | 19,218 |
| dev/source-python | C-dict-9-D256k-Tall | 597.6 | 593.8 | 334.8 | 210.8 | 69.6 | 1.48 / 1.67 | 30.3 / 19.2 | 262,144 | 19,218 |
| dev/source-python | C-dict-9-D64k-T1 | 323.8 | 328.1 | 74.3 | 190.8 | 67.3 | 1.42 / 1.76 | 23.5 / 18.7 | 65,536 | 19,218 |
| dev/source-python | C-dict-9-D64k-T4 | 452.4 | 453.1 | 198.5 | 191.5 | 68.9 | 1.02 / 1.22 | 26.6 / 18.7 | 65,536 | 19,218 |
| dev/source-python | C-dict-9-D64k-Tall | 458.5 | 453.1 | 221.2 | 182.9 | 65.7 | 1.25 / 1.62 | 27.5 / 18.7 | 65,536 | 19,218 |
| dev/source-python | C-nodict-3 | 149.7 | 140.6 | 0.0 | 81.4 | 68.1 | 1.28 / 1.52 | 15.2 / 18.8 | 0 | 19,218 |
| dev/source-python | C-nodict-9 | 224.2 | 218.8 | 0.0 | 167.8 | 67.5 | 1.20 / 1.53 | 15.7 / 18.7 | 0 | 19,218 |

## Per-file stored bytes vs same-level no-dictionary container

| dataset | config | files +/0/− | mean | median | p10 | p90 | worst |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| dev/config-cmake-pkg | C-dict-3-D112k-T1 | 1245/175/11 | 223.0 | 131 | 0 | 474 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-370), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-94) |
| dev/config-cmake-pkg | C-dict-3-D112k-T4 | 1276/151/4 | 216.0 | 133 | 0 | 543 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-407), msys2-etc/localtime (-75) |
| dev/config-cmake-pkg | C-dict-3-D112k-Tall | 1276/151/4 | 216.0 | 133 | 0 | 543 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-407), msys2-etc/localtime (-75) |
| dev/config-cmake-pkg | C-dict-3-D16k-T1 | 1247/174/10 | 151.4 | 121 | 0 | 356 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-341), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-109) |
| dev/config-cmake-pkg | C-dict-3-D16k-T4 | 1257/167/7 | 163.8 | 124 | 0 | 399 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-199), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-98) |
| dev/config-cmake-pkg | C-dict-3-D16k-Tall | 1257/167/7 | 163.8 | 124 | 0 | 399 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-199), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-98) |
| dev/config-cmake-pkg | C-dict-3-D256k-T1 | 1267/156/8 | 292.9 | 138 | 0 | 588 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-104), msys2-etc/pacman.d/gnupg/pubring.gpg (-86) |
| dev/config-cmake-pkg | C-dict-3-D256k-T4 | 1288/140/3 | 242.0 | 136 | 6 | 600 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.OSXX11.main.scpt.in (-42), msys2-etc/pacman.d/gnupg/private-keys-v1.d/A504F79A0B7DB0C77BC36919A982901407988DF0.key (-25) |
| dev/config-cmake-pkg | C-dict-3-D256k-Tall | 1288/140/3 | 242.0 | 136 | 6 | 600 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.OSXX11.main.scpt.in (-42), msys2-etc/pacman.d/gnupg/private-keys-v1.d/A504F79A0B7DB0C77BC36919A982901407988DF0.key (-25) |
| dev/config-cmake-pkg | C-dict-3-D64k-T1 | 1235/183/13 | 196.8 | 125 | 0 | 463 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-359), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.DS_Store.in (-95) |
| dev/config-cmake-pkg | C-dict-3-D64k-T4 | 1267/158/6 | 199.4 | 129 | 0 | 515 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-390), msys2-etc/localtime (-76) |
| dev/config-cmake-pkg | C-dict-3-D64k-Tall | 1267/158/6 | 199.4 | 129 | 0 | 515 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-390), msys2-etc/localtime (-76) |
| dev/config-cmake-pkg | C-dict-9-D112k-T1 | 1267/159/5 | 254.7 | 137 | 0 | 580 | msys2-etc/pacman.d/gnupg/trustdb.gpg (-15), msys2-etc/pacman.d/gnupg/private-keys-v1.d/A504F79A0B7DB0C77BC36919A982901407988DF0.key (-11) |
| dev/config-cmake-pkg | C-dict-9-D112k-T4 | 1287/142/2 | 249.7 | 138 | 0 | 637 | msys2-etc/localtime (-5), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-3) |
| dev/config-cmake-pkg | C-dict-9-D112k-Tall | 1287/142/2 | 249.7 | 138 | 0 | 637 | msys2-etc/localtime (-5), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-3) |
| dev/config-cmake-pkg | C-dict-9-D16k-T1 | 1258/170/3 | 169.7 | 124 | 0 | 422 | msys2-etc/pacman.d/gnupg/trustdb.gpg (-15), msys2-etc/localtime (-4) |
| dev/config-cmake-pkg | C-dict-9-D16k-T4 | 1261/169/1 | 181.3 | 126 | 0 | 441 | msys2-etc/localtime (-2) |
| dev/config-cmake-pkg | C-dict-9-D16k-Tall | 1261/169/1 | 181.3 | 126 | 0 | 441 | msys2-etc/localtime (-2) |
| dev/config-cmake-pkg | C-dict-9-D256k-T1 | 1280/147/4 | 319.9 | 141 | 0 | 710 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-1202), msys2-etc/pacman.d/gnupg/trustdb.gpg (-38) |
| dev/config-cmake-pkg | C-dict-9-D256k-T4 | 1300/130/1 | 293.0 | 141 | 13 | 736 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-4) |
| dev/config-cmake-pkg | C-dict-9-D256k-Tall | 1300/130/1 | 293.0 | 141 | 13 | 736 | cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-4) |
| dev/config-cmake-pkg | C-dict-9-D64k-T1 | 1265/162/4 | 225.8 | 131 | 0 | 566 | msys2-etc/pacman.d/gnupg/trustdb.gpg (-15), msys2-etc/pacman.d/gnupg/private-keys-v1.d/A504F79A0B7DB0C77BC36919A982901407988DF0.key (-6) |
| dev/config-cmake-pkg | C-dict-9-D64k-T4 | 1274/155/2 | 227.4 | 134 | 0 | 591 | msys2-etc/localtime (-7), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-3) |
| dev/config-cmake-pkg | C-dict-9-D64k-Tall | 1274/155/2 | 227.4 | 134 | 0 | 591 | msys2-etc/localtime (-7), cmake-share/cmake-4.4/Modules/Internal/CPack/CPack.VolumeIcon.icns.in (-3) |
| dev/many-small-docs | C-dict-3-D112k-T1 | 746/0/0 | 253.3 | 209 | 91 | 425 | — |
| dev/many-small-docs | C-dict-3-D112k-T4 | 746/0/0 | 253.3 | 209 | 91 | 425 | — |
| dev/many-small-docs | C-dict-3-D112k-Tall | 746/0/0 | 253.3 | 209 | 91 | 425 | — |
| dev/many-small-docs | C-dict-3-D16k-T1 | 746/0/0 | 171.5 | 154 | 73 | 283 | — |
| dev/many-small-docs | C-dict-3-D16k-T4 | 746/0/0 | 171.5 | 154 | 73 | 283 | — |
| dev/many-small-docs | C-dict-3-D16k-Tall | 746/0/0 | 171.5 | 154 | 73 | 283 | — |
| dev/many-small-docs | C-dict-3-D256k-T1 | 746/0/0 | 316.8 | 239 | 106 | 558 | — |
| dev/many-small-docs | C-dict-3-D256k-T4 | 746/0/0 | 316.8 | 239 | 106 | 558 | — |
| dev/many-small-docs | C-dict-3-D256k-Tall | 746/0/0 | 316.8 | 239 | 106 | 558 | — |
| dev/many-small-docs | C-dict-3-D64k-T1 | 746/0/0 | 225.1 | 188 | 84 | 371 | — |
| dev/many-small-docs | C-dict-3-D64k-T4 | 746/0/0 | 225.1 | 188 | 84 | 371 | — |
| dev/many-small-docs | C-dict-3-D64k-Tall | 746/0/0 | 225.1 | 188 | 84 | 371 | — |
| dev/many-small-docs | C-dict-9-D112k-T1 | 746/0/0 | 267.3 | 223 | 91 | 448 | — |
| dev/many-small-docs | C-dict-9-D112k-T4 | 746/0/0 | 267.3 | 223 | 91 | 448 | — |
| dev/many-small-docs | C-dict-9-D112k-Tall | 746/0/0 | 267.3 | 223 | 91 | 448 | — |
| dev/many-small-docs | C-dict-9-D16k-T1 | 746/0/0 | 181.6 | 162 | 72 | 298 | — |
| dev/many-small-docs | C-dict-9-D16k-T4 | 746/0/0 | 181.6 | 162 | 72 | 298 | — |
| dev/many-small-docs | C-dict-9-D16k-Tall | 746/0/0 | 181.6 | 162 | 72 | 298 | — |
| dev/many-small-docs | C-dict-9-D256k-T1 | 746/0/0 | 330.0 | 253 | 111 | 564 | — |
| dev/many-small-docs | C-dict-9-D256k-T4 | 746/0/0 | 330.0 | 253 | 111 | 564 | — |
| dev/many-small-docs | C-dict-9-D256k-Tall | 746/0/0 | 330.0 | 253 | 111 | 564 | — |
| dev/many-small-docs | C-dict-9-D64k-T1 | 746/0/0 | 238.3 | 200 | 84 | 392 | — |
| dev/many-small-docs | C-dict-9-D64k-T4 | 746/0/0 | 238.3 | 200 | 84 | 392 | — |
| dev/many-small-docs | C-dict-9-D64k-Tall | 746/0/0 | 238.3 | 200 | 84 | 392 | — |
| dev/source-python | C-dict-3-D112k-T1 | 348/0/1 | 651.2 | 296 | 90 | 1,593 | python-lib/re/_casefix.py (-40) |
| dev/source-python | C-dict-3-D112k-T4 | 345/2/2 | 670.4 | 310 | 107 | 1,679 | python-lib/re/_casefix.py (-56), python-lib/pydoc_data/module_docs.py (-33) |
| dev/source-python | C-dict-3-D112k-Tall | 346/1/2 | 680.7 | 336 | 127 | 1,608 | python-lib/re/_casefix.py (-86), python-lib/pydoc_data/module_docs.py (-17) |
| dev/source-python | C-dict-3-D16k-T1 | 335/6/8 | 289.4 | 74 | 17 | 993 | python-lib/re/_casefix.py (-62), python-lib/pydoc_data/module_docs.py (-49) |
| dev/source-python | C-dict-3-D16k-T4 | 342/3/4 | 331.9 | 143 | 47 | 1,156 | python-lib/re/_casefix.py (-58), python-lib/pydoc_data/module_docs.py (-47) |
| dev/source-python | C-dict-3-D16k-Tall | 342/4/3 | 346.4 | 160 | 59 | 1,166 | python-lib/re/_casefix.py (-70), python-lib/pydoc_data/module_docs.py (-55) |
| dev/source-python | C-dict-3-D256k-T1 | 348/0/1 | 416.8 | 259 | 98 | 809 | python-lib/encodings/aliases.py (-46) |
| dev/source-python | C-dict-3-D256k-T4 | 347/1/1 | 461.2 | 335 | 121 | 869 | python-lib/pydoc_data/module_docs.py (-25) |
| dev/source-python | C-dict-3-D256k-Tall | 348/0/1 | 892.6 | 508 | 158 | 2,079 | python-lib/pydoc_data/module_docs.py (-106) |
| dev/source-python | C-dict-3-D64k-T1 | 346/2/1 | 585.1 | 254 | 79 | 1,628 | python-lib/re/_casefix.py (-54) |
| dev/source-python | C-dict-3-D64k-T4 | 346/1/2 | 570.2 | 275 | 81 | 1,672 | python-lib/re/_casefix.py (-49), python-lib/pydoc_data/module_docs.py (-41) |
| dev/source-python | C-dict-3-D64k-Tall | 346/1/2 | 588.1 | 285 | 93 | 1,669 | python-lib/re/_casefix.py (-86), python-lib/pydoc_data/module_docs.py (-46) |
| dev/source-python | C-dict-9-D112k-T1 | 348/1/0 | 803.0 | 398 | 133 | 1,859 | — |
| dev/source-python | C-dict-9-D112k-T4 | 348/1/0 | 806.5 | 435 | 140 | 1,831 | — |
| dev/source-python | C-dict-9-D112k-Tall | 346/3/0 | 811.9 | 456 | 146 | 1,781 | — |
| dev/source-python | C-dict-9-D16k-T1 | 343/6/0 | 380.4 | 154 | 61 | 1,252 | — |
| dev/source-python | C-dict-9-D16k-T4 | 343/6/0 | 428.1 | 207 | 82 | 1,284 | — |
| dev/source-python | C-dict-9-D16k-Tall | 343/6/0 | 451.3 | 231 | 85 | 1,340 | — |
| dev/source-python | C-dict-9-D256k-T1 | 348/1/0 | 674.4 | 427 | 149 | 1,344 | — |
| dev/source-python | C-dict-9-D256k-T4 | 348/1/0 | 775.0 | 542 | 185 | 1,584 | — |
| dev/source-python | C-dict-9-D256k-Tall | 346/3/0 | 1,031.9 | 630 | 188 | 2,189 | — |
| dev/source-python | C-dict-9-D64k-T1 | 345/4/0 | 701.1 | 324 | 117 | 1,700 | — |
| dev/source-python | C-dict-9-D64k-T4 | 346/3/0 | 685.2 | 341 | 116 | 1,729 | — |
| dev/source-python | C-dict-9-D64k-Tall | 347/2/0 | 704.6 | 373 | 122 | 1,790 | — |

## Selection (registered rule)

- level 3: C-dict-3-D64k-Tall (dev total 3,442,393 B)
- level 9: C-dict-9-D64k-Tall (dev total 3,141,686 B)
