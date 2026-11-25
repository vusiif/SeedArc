# DEV-023 observation tables

Generated from `analysis.json` by `bounded_expand_preview.py`; all audits ok: True. Times are four-round min–median–max on one pinned CPU, warm cache. "Read/decode" is the access-profile diagnostic (whole archive read + target frame decode + byte check, nothing written).

## source-added10 — 11285 files, 268,433,674 B, added 26,841,670 B (10.00%)

| Frame limit | Op | Archive B | vs whole | Frames | Encode s | Read/decode median ms | Same-round read speedup | Append/fresh wall | Append size vs fresh |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| whole (1 GiB) | fresh | 28,141,155 | +0.000% | 1 | 10.78–11.03–11.25 | 479.8–496.2–511.4 | 1.00–1.00–1.00× | – | – |
| whole (1 GiB) | append | 29,227,210 | +0.000% | 2 | 3.44–3.50–4.36 | 448.4–459.8–488.4 | 1.00–1.00–1.00× | 0.305–0.317–0.405 | +3.859% |
| 32 MiB | fresh | 28,410,983 | +0.959% | 9 | 10.40–10.49–10.75 | 145.1–163.9–193.1 | 2.65–3.03–3.31× | – | – |
| 32 MiB | append | 29,454,858 | +0.779% | 9 | 3.32–3.36–3.49 | 146.9–153.9–164.4 | 2.82–2.97–3.26× | 0.313–0.318–0.336 | +3.674% |
| 64 MiB | fresh | 28,301,832 | +0.571% | 5 | 10.50–10.77–10.89 | 203.8–210.8–235.7 | 2.15–2.35–2.38× | – | – |
| 64 MiB | append | 29,368,465 | +0.483% | 5 | 3.44–3.48–3.53 | 213.3–217.1–222.3 | 2.08–2.09–2.27× | 0.317–0.323–0.336 | +3.769% |
| 128 MiB | fresh | 28,279,054 | +0.490% | 3 | 10.91–10.92–11.12 | 289.7–296.3–308.7 | 1.57–1.68–1.73× | – | – |
| 128 MiB | append | 29,333,088 | +0.362% | 3 | 3.43–3.50–3.61 | 283.8–292.1–305.9 | 1.52–1.60–1.61× | 0.312–0.318–0.331 | +3.727% |

| Frame limit | Parent B | Parent seed pack s | Old archive read = copied B | Old plaintext recompressed | Fault: wrong bytes accepted (fresh / append) | Fault verified bytes at 10/50/90% (append) |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| whole (1 GiB) | 26,775,553 | 10.55 | 26,775,553 | 0 | 0 / 0 | 268,433,137 / 266,830,367 / 268,393,542 of 268,433,674 |
| 32 MiB | 27,002,998 | 9.94 | 27,002,998 | 0 | 0 / 0 | 268,338,604 / 266,173,037 / 242,605,872 of 268,433,674 |
| 64 MiB | 26,916,721 | 9.83 | 26,916,721 | 0 | 0 / 0 | 210,471,049 / 201,336,801 / 267,737,167 of 268,433,674 |
| 128 MiB | 26,881,402 | 10.42 | 26,881,402 | 0 | 0 / 0 | 140,077,286 / 268,323,552 / 155,198,058 of 268,433,674 |

Read/decode samples: `linux/Documentation/features/list-arch.sh`, `linux/arch/mips/include/asm/mach-loongson2ef/cs5536/cs5536_mfgpt.h`, `linux/arch/sh/kernel/cpu/sh4a/clock-sh7770.c`, `linux/drivers/clk/hisilicon/clk-hi3519.c`, `linux/drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_7_2_0_sh_mask.h`, `linux/drivers/gpu/drm/amd/pm/powerplay/hwmgr/smu10_hwmgr.c`, `linux/drivers/gpu/drm/vc4/vc4_qpu_defines.h`, `linux/drivers/media/pci/smipcie/smipcie.h`, `linux/drivers/tty/serial/8250/8250_ingenic.c`, `linux/include/linux/mfd/si476x-core.h`, `linux/net/appletalk/ddp.c`, `linux/tools/perf/arch/s390/util/perf_regs.c`, `linux/virt/lib/irqbypass.c`

## source-added40 — 11285 files, 268,433,674 B, added 107,364,961 B (40.00%)

| Frame limit | Op | Archive B | vs whole | Frames | Encode s | Read/decode median ms | Same-round read speedup | Append/fresh wall | Append size vs fresh |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| whole (1 GiB) | fresh | 28,141,155 | +0.000% | 1 | 10.93–11.23–11.45 | 464.7–482.1–493.8 | 1.00–1.00–1.00× | – | – |
| whole (1 GiB) | append | 30,035,647 | +0.000% | 2 | 6.09–6.29–6.47 | 325.5–340.7–380.5 | 1.00–1.00–1.00× | 0.532–0.560–0.592 | +6.732% |
| 32 MiB | fresh | 28,410,983 | +0.959% | 9 | 10.67–10.82–11.45 | 142.6–153.3–170.7 | 2.72–3.16–3.44× | – | – |
| 32 MiB | append | 30,191,139 | +0.518% | 9 | 5.89–6.00–6.10 | 157.0–164.6–170.7 | 1.91–2.07–2.42× | 0.533–0.552–0.557 | +6.266% |
| 64 MiB | fresh | 28,301,832 | +0.571% | 5 | 10.72–10.89–11.33 | 206.8–215.0–229.3 | 2.07–2.26–2.31× | – | – |
| 64 MiB | append | 30,148,806 | +0.377% | 5 | 5.76–6.14–6.28 | 179.7–189.4–198.5 | 1.78–1.81–1.92× | 0.529–0.552–0.586 | +6.526% |
| 128 MiB | fresh | 28,279,054 | +0.490% | 3 | 10.46–10.78–11.32 | 283.1–298.7–332.8 | 1.42–1.64–1.66× | – | – |
| 128 MiB | append | 30,051,911 | +0.054% | 3 | 5.96–6.15–6.55 | 237.1–266.8–287.4 | 1.23–1.29–1.45× | 0.526–0.573–0.620 | +6.269% |

| Frame limit | Parent B | Parent seed pack s | Old archive read = copied B | Old plaintext recompressed | Fault: wrong bytes accepted (fresh / append) | Fault verified bytes at 10/50/90% (append) |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| whole (1 GiB) | 19,034,314 | 7.18 | 19,034,314 | 0 | 0 / 0 | 107,364,961 / 268,403,164 / 161,068,713 of 268,433,674 |
| 32 MiB | 19,102,796 | 6.98 | 19,102,796 | 0 | 0 / 0 | 236,519,937 / 268,408,611 / 268,174,488 of 268,433,674 |
| 64 MiB | 19,067,698 | 7.18 | 19,067,698 | 0 | 0 / 0 | 201,955,955 / 240,938,455 / 267,657,032 of 268,433,674 |
| 128 MiB | 19,050,549 | 7.27 | 19,050,549 | 0 | 0 / 0 | 268,393,232 / 241,562,663 / 161,068,713 of 268,433,674 |

Read/decode samples: `linux/Documentation/features/list-arch.sh`, `linux/arch/mips/include/asm/mach-loongson2ef/cs5536/cs5536_mfgpt.h`, `linux/arch/sh/kernel/cpu/sh4a/clock-sh7770.c`, `linux/drivers/clk/hisilicon/clk-hi3519.c`, `linux/drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_7_2_0_sh_mask.h`, `linux/drivers/gpu/drm/amd/pm/powerplay/hwmgr/smu10_hwmgr.c`, `linux/drivers/gpu/drm/vc4/vc4_qpu_defines.h`, `linux/drivers/media/pci/smipcie/smipcie.h`, `linux/drivers/tty/serial/8250/8250_ingenic.c`, `linux/include/linux/mfd/si476x-core.h`, `linux/net/appletalk/ddp.c`, `linux/tools/perf/arch/s390/util/perf_regs.c`, `linux/virt/lib/irqbypass.c`

## text-added10 — 49 files, 81,553,445 B, added 8,000,000 B (9.81%)

| Frame limit | Op | Archive B | vs whole | Frames | Encode s | Read/decode median ms | Same-round read speedup | Append/fresh wall | Append size vs fresh |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| whole (1 GiB) | fresh | 17,006,369 | +0.000% | 1 | 4.38–4.41–4.69 | 191.1–203.6–214.3 | 1.00–1.00–1.00× | – | – |
| whole (1 GiB) | append | 17,075,052 | +0.000% | 2 | 1.28–1.32–1.39 | 175.1–178.0–184.6 | 1.00–1.00–1.00× | 0.291–0.298–0.304 | +0.404% |
| 32 MiB | fresh | 17,065,581 | +0.348% | 3 | 4.23–4.37–5.08 | 88.2–93.5–105.8 | 1.93–2.19–2.25× | – | – |
| 32 MiB | append | 17,124,293 | +0.288% | 4 | 1.26–1.27–1.32 | 85.6–89.2–92.2 | 1.90–2.04–2.06× | 0.260–0.291–0.299 | +0.344% |
| 64 MiB | fresh | 17,003,161 | -0.019% | 2 | 4.14–4.21–4.75 | 140.6–148.9–170.5 | 1.26–1.33–1.44× | – | – |
| 64 MiB | append | 17,072,283 | -0.016% | 3 | 1.26–1.31–1.33 | 113.4–124.5–133.8 | 1.38–1.43–1.54× | 0.280–0.305–0.314 | +0.407% |
| 128 MiB | fresh | 17,006,369 | +0.000% | 1 | 4.37–4.50–4.76 | 201.9–212.0–224.5 | 0.87–0.95–1.06× | – | – |
| 128 MiB | append | 17,075,052 | +0.000% | 2 | 1.28–1.31–1.35 | 177.1–178.8–190.2 | 0.92–0.99–1.04× | 0.281–0.292–0.295 | +0.404% |

| Frame limit | Parent B | Parent seed pack s | Old archive read = copied B | Old plaintext recompressed | Fault: wrong bytes accepted (fresh / append) | Fault verified bytes at 10/50/90% (append) |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| whole (1 GiB) | 14,557,584 | 3.99 | 14,557,584 | 0 | 0 / 0 | 8,000,000 / 80,553,445 / 73,553,445 of 81,553,445 |
| 32 MiB | 14,606,767 | 3.85 | 14,606,767 | 0 | 0 / 0 | 80,553,445 / 79,553,445 / 73,553,445 of 81,553,445 |
| 64 MiB | 14,554,786 | 3.75 | 14,554,786 | 0 | 0 / 0 | 80,553,445 / 41,553,445 / 76,553,445 of 81,553,445 |
| 128 MiB | 14,557,584 | 3.84 | 14,557,584 | 0 | 0 / 0 | 8,000,000 / 80,553,445 / 73,553,445 of 81,553,445 |

Read/decode samples: `enwik8/part-000.xml`, `enwik8/part-005.xml`, `enwik8/part-009.xml`, `enwik8/part-014.xml`, `enwik8/part-018.xml`, `enwik8/part-022.xml`, `enwik8/part-027.xml`, `enwik8/part-031.xml`, `enwik8/part-036.xml`, `enwik8/part-041.xml`, `enwik8/part-045.xml`, `silesia/nci`

## text-added40 — 49 files, 81,553,445 B, added 33,000,000 B (40.46%)

| Frame limit | Op | Archive B | vs whole | Frames | Encode s | Read/decode median ms | Same-round read speedup | Append/fresh wall | Append size vs fresh |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| whole (1 GiB) | fresh | 17,006,369 | +0.000% | 1 | 4.54–4.67–5.17 | 193.8–203.2–245.9 | 1.00–1.00–1.00× | – | – |
| whole (1 GiB) | append | 17,084,467 | +0.000% | 2 | 2.66–2.75–3.57 | 101.5–106.4–155.9 | 1.00–1.00–1.00× | 0.573–0.596–0.690 | +0.459% |
| 32 MiB | fresh | 17,065,581 | +0.348% | 3 | 4.16–4.66–4.98 | 82.2–93.1–102.5 | 1.91–2.26–2.78× | – | – |
| 32 MiB | append | 17,081,919 | -0.015% | 3 | 2.54–2.85–3.07 | 90.9–105.7–111.5 | 0.96–1.09–1.40× | 0.568–0.610–0.670 | +0.096% |
| 64 MiB | fresh | 17,003,161 | -0.019% | 2 | 4.44–4.51–5.44 | 141.7–163.2–166.5 | 1.16–1.34–1.53× | – | – |
| 64 MiB | append | 17,084,467 | +0.000% | 2 | 2.57–2.71–3.08 | 85.9–95.0–105.7 | 0.96–1.15–1.72× | 0.565–0.581–0.621 | +0.478% |
| 128 MiB | fresh | 17,006,369 | +0.000% | 1 | 4.46–4.66–4.77 | 204.0–214.1–274.5 | 0.90–0.91–1.04× | – | – |
| 128 MiB | append | 17,084,467 | +0.000% | 2 | 2.66–2.94–3.08 | 96.9–107.5–110.5 | 0.92–0.99–1.61× | 0.583–0.632–0.659 | +0.459% |

| Frame limit | Parent B | Parent seed pack s | Old archive read = copied B | Old plaintext recompressed | Fault: wrong bytes accepted (fresh / append) | Fault verified bytes at 10/50/90% (append) |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| whole (1 GiB) | 6,852,555 | 2.07 | 6,852,555 | 0 | 0 / 0 | 80,553,445 / 48,553,445 / 76,553,445 of 81,553,445 |
| 32 MiB | 6,849,978 | 2.10 | 6,849,978 | 0 | 0 / 0 | 80,553,445 / 48,553,445 / 48,553,445 of 81,553,445 |
| 64 MiB | 6,852,555 | 2.08 | 6,852,555 | 0 | 0 / 0 | 80,553,445 / 48,553,445 / 76,553,445 of 81,553,445 |
| 128 MiB | 6,852,555 | 2.13 | 6,852,555 | 0 | 0 / 0 | 80,553,445 / 48,553,445 / 76,553,445 of 81,553,445 |

Read/decode samples: `enwik8/part-000.xml`, `enwik8/part-005.xml`, `enwik8/part-009.xml`, `enwik8/part-014.xml`, `enwik8/part-018.xml`, `enwik8/part-022.xml`, `enwik8/part-027.xml`, `enwik8/part-031.xml`, `enwik8/part-036.xml`, `enwik8/part-041.xml`, `enwik8/part-045.xml`, `silesia/nci`

