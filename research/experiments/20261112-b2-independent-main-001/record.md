# B2-r5 P1：冻结 Adaptive-file-r1 的独立语料评估与 Seed 消融（首轮）

日期：2026-11-12（America/Los_Angeles），运行时间 17:46–18:09。任务 DEV-012，主 Agent 执行（由 B2 协调会话委派）。依据：[项目评估与发展建议](../../../docs/review_results/SeedArc_项目评估与发展建议_2026-11-12.md) 第 7 节。冻结配置：[adaptive-file-r1.frozen.json](../../configs/adaptive-file-r1.frozen.json)（本目录另存一份副本）。消融设计：[seed-ablation-r1](../../ideas/seed-ablation-r1.md)。语料：[b2r5-corpus-r1](../../datasets/b2r5-corpus-r1.md)。

## 结论

在独立测试集上（10 类数据、约 89.5 MB、4,731 个文件，冻结后没有调过参数）：

1. **Adaptive-file-r1 从未让归档变大**：10 个数据集、4 个预算档，逐数据集、逐文件都没有出现负收益。
2. **但它被 Fixed LZMA2-6 全面压过**：无界档合计 43,820,241 B，比 Zstd-3 小 7.45 %；Fixed LZMA2-6 是 43,583,688 B（−7.95 %），更小，而且压缩更快（合计墙钟 13.6 s，无界档 14.5 s）。256 KiB / 1 MiB 两档几乎全部耗尽预算，只省 0.09 % / 0.66 %。
3. **Oracle-file 只比 Fixed LZMA2-6 再小 10,927 B（0.025 %）**，压缩耗时却要 24.5 s。所以在这份语料上，“按文件选 Codec”能多拿的空间几乎是零。
4. **有 Seed、无搜索的 B-r1 seed0 比 Zstd-3 大 26.1 %**，10 类数据全部变大；原因与 r4 一样，是与内容无关地把块计划成 Store。
5. **Seed 的独立价值本轮无法判定**：G5（有 Seed + 有限预算搜索）没有实现，按要求本轮也不新建搜索器。

**对 r2 的建议（只是建议，本轮未实施，须先在 dev 划分上验证）**：

- 默认 Codec 或候选顺序应该考虑 LZMA2-6。在这份语料上，固定 LZMA2-6 的大小和速度都优于 r1。
- `below_min_size` 让大量小文件放弃了 LZMA2 的收益，terminfo 是典型：LZMA2-6 能省 11.2 %，r1 是 0。
- 预算单位目前是试编码字节，已压缩数据上会浪费掉（compressed-packages 试编码了 8.6 MB，一字节未省）。应当先用廉价的熵或特征判断，并改为按时间计预算。
- 这些属于渐进抽样 / 时间预算 / 跳过机制的 P1 议题，本轮不做。

## 方法

- **语料**：`b2r5-corpus-r1` 的 test 划分。划分在任何压缩运行之前就已固定（按相对路径哈希）。运行前逐文件核对 SHA256 与清单一致。dev 划分没有读取。
- **方案**（G 编号对应消融设计中的组）：
  - G1 Fixed Zstd-3；
  - G2 Fixed LZMA2-6；
  - G3 Adaptive-file-r1（冻结版，预算 256 KiB / 1 MiB / 无界）；
  - G4 B-r1 seed0（有 Seed、无搜索）；
  - G6 Oracle-file（每文件全部块 × 3 种配置，作为参考上限）。
  - 共 7 个方案，固定 64 KiB 分块，单线程。
- **重复与顺序**：每组 5 次重复，方案顺序每次轮换一位，数据集按名称排序。
- **缓存**：每个数据集在第一次测量前完整读一遍（热缓存），不重启、不清缓存。
- **每次运行的检查**：compress → verify → extract，逐字节比较；共 350 次，全部一致，同组 5 次的归档 SHA256 全部相同。
- **计量口径**：
  - 完整归档字节 = 头部 + 索引 + 数据。G1–G3 和 G6 的选择结果完全由逐块 Codec 表达，元数据 0 B；G4 的 Seed 尚未序列化，记为缺项，不记 0。
  - 时间：工具内 compress / extract 调用的墙钟与 CPU（process user+kernel，粒度约 15.6 ms），决策时间在规划器内单独计时。表中取 5 次的中位数，summary.json 另有均值、p10、p90、最小、最大值。
  - 内存：每个进程的 PeakWorkingSetSize，包含工具本身和 JSON 输出。
- **工具**：二进制 SHA256 `7a37a5d6…b7ff`，是 r5 的最终版本。选择器与工具源码与冻结配置记录的哈希一致（提交 `4cea9e2`）；本 run 的未提交改动只涉及测试与脚本（见 tracked-source.patch / git-status.txt）。

## 结果

完整表格见 [preview.md](preview.md)，原始指标见 summary.json。

### 合计（10 个测试集）

| 方案 | 归档 B | 相对 Zstd-3 | 压缩墙钟 ms | 压缩 CPU ms | 其中决策 ms | 解压墙钟 ms | 试编码输入 B | 预算耗尽文件 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| G1 Fixed Zstd-3 | 47,347,852 | 0 | 2,682 | 2,641 | 0 | 8,071 | 0 | 0 |
| G2 Fixed LZMA2-6 | 43,583,688 | −7.95 % | 13,621 | 13,406 | 0 | 8,903 | 0 | 0 |
| G3 Adaptive-r1 @256 KiB | 47,304,401 | −0.09 % | 3,020 | 2,969 | 255 | 7,958 | 2,177,288 | 471 |
| G3 Adaptive-r1 @1 MiB | 47,033,880 | −0.66 % | 3,977 | 3,922 | 602 | 8,177 | 9,139,303 | 441 |
| G3 Adaptive-r1 @无界 | 43,820,241 | −7.45 % | 14,517 | 14,437 | 4,760 | 8,727 | 110,106,387 | 0 |
| G6 Oracle-file | 43,572,761 | −7.97 % | 24,537 | 24,094 | 11,805 | 9,046 | 268,457,442 | 0 |
| G4 B-r1 seed0 | 59,708,345 | +26.11 % | 6,726 | 6,719 | 0 | 8,376 | 0 | 0 |

- 预算耗尽的文件数按数据集累加，同一文件在不同数据集里不会重复计数。
- 无界档的试编码输入（110 MB）超过了语料总量（89.5 MB）。原因：一到三块的小文件，抽样就等于整个文件，还要试 2–3 种配置。

### 按数据集（相对 Zstd-3 的大小变化，分布在 summary.json）

| 方案 | 均值 | 中位数 | p10 | p90 |
| --- | ---: | ---: | ---: | ---: |
| G2 Fixed LZMA2-6 | −10.43 % | −9.64 % | −18.95 % | −0.17 % |
| G3 @无界 | −8.56 % | −6.45 % | −18.94 % | 0.00 % |
| G6 Oracle | −10.50 % | −9.70 % | −18.95 % | −0.18 % |
| G4 B-r1 seed0 | +68.66 % | +38.22 % | +0.66 % | +109.90 % |

### 负收益与无收益样本

- **G3 各档**：没有任何数据集、也没有任何文件比 Zstd-3 大。
  - 无界档没有收益的两组：many-small-terminfo（1,477 个文件全部低于 16 KiB，判定 `below_min_size`；而 LZMA2-6 能省 112,709 B）和 compressed-packages（26 个 `no_gain`、4 个 `insufficient_gain`，试编码 8.6 MB 而一字节未省，多花 437 ms CPU）。
  - 256 KiB / 1 MiB 两档在多数数据集上预算耗尽（分别 471 / 441 个文件），收益很小。
- **G2 Fixed LZMA2-6 的逐文件负收益**：数据集合计都在变小，但个别文件变大：config-cmake 864 个小文件（每个最多 +28 B）、source-headers 62 个、duplicate-licenses 13 个、compressed-packages 1 个（+203 B）、image-jpeg 1 个（+376 B）。
- **G4 B-r1 seed0**：10 个数据集全部变大，变大的文件共 1,960 个；最大的一个是 `mdmgl004.inf`，比 Zstd-3 多 499,728 B。

### 每节省 1 KiB 的额外压缩 CPU（相对 Zstd-3，ms/KiB，取中位数）

| 类型 | G2 LZMA2-6 | G3 @无界 | G6 Oracle |
| --- | ---: | ---: | ---: |
| 音频 PCM | 1.14 | 1.74 | 2.30 |
| 原生二进制 | 1.54 | 1.97 | 3.22 |
| 结构化 INF | 4.08 | 7.28 | 8.74 |
| 源码 | 4.67 | 8.06 | 9.93 |
| 大量小文件 | 4.68 | 无收益 | 10.08 |
| 日志 | 5.67 | 11.63 | 12.05 |
| 跨文件重复 | 5.66 | 9.69 | 12.75 |
| 配置 / 文本 | 8.18 | 8.11 | 17.02 |
| 图片 JPEG | 68.12 | 33.02 | 96.47 |
| 已压缩 zstd 包 | 295.76 | 无收益（+437 ms） | 433.73 |

- G3 只在配置 / 文本和图片两类上，单位 CPU 成本低于 LZMA2-6；而在这两类上，它节省的字节也只有 LZMA2-6 的 67 % / 55 %。
- 解压：LZMA2 比 Zstd 慢一些，合计 8.9 s 对 8.1 s。G3 无界档的解压合计是 8.7 s。各方案的解压峰值工作集都在约 7–9 MiB。

## 消融（对照 seed-ablation-r1 的判定规则）

| 组 | 状态 | 结果 |
| --- | --- | --- |
| G1 固定 Zstd-3 | 已运行 | 基线 |
| G2 固定 LZMA2-6 | 已运行 | −7.95 %，速度约为 Zstd-3 的 1/5 |
| G3 无 Seed 启发式 | 已运行 | 节省超过 0.1 %，判定为“有净收益”；但空间和时间都被 G2 压过 |
| G4 有 Seed、无搜索 | 已运行 | 10 类全部负收益（+26.1 %），符合“无内容依据的映射为负收益” |
| G5 有 Seed + 有限预算搜索 | **缺项** | 没有实现；本轮不新建搜索器，因此不对 Seed 的独立价值下结论 |
| G6 Oracle | 已运行（参考上限） | 只比 G2 小 0.025 % |

G4 的 Seed / 覆盖字段尚未序列化，元数据成本是缺项。即便按 0 B 计，G4 也已经最差。

## 限制

- 只有一台机器、一个 Windows 环境、热缓存、5 次重复，结论不外推到其他平台或冷缓存。
- 语料来自本机软件和系统文件，代表性有限。没有视频样本。日志与 INF 文件不可再分发，他人只能按同样的规则在自己机器上重建同类语料，不能逐字节复现。
- CPU 计时粒度约 15.6 ms，小数据集上的 ms/KiB 只能看量级。
- 只覆盖了 64 KiB 分块、单线程、文件级选择，没有评估跨文件字典或固实压缩（P1-1 方向）。

## 复现

```powershell
python research/scripts/b2_corpus.py --corpus-id b2r5-corpus-r1        # 已运行；重建时请用新 ID 并核对 TSV
python research/scripts/b2_independent.py --tool build/msvc-vs2026-release/research/tools/Release/seedarc_seed_archive.exe --run-id 20261112-b2-independent-main-001 --repetitions 5
python research/scripts/b2_independent.py --analyze-only research/experiments/20261112-b2-independent-main-001
```

- 本目录包含：脚本副本、冻结配置副本、config.json（含工具、脚本、清单、冻结配置的 SHA256）、raw.json（精简版）、per_file.csv（第 0 次重复的逐文件存储字节）、summary.json、preview.md、logs/（每次操作的日志，已截断）。
- 完整的 350 次工具输出在 `research/local/…/raw.full.json`，哈希见 raw-full.sha256.json；第 0 次重复的完整块明细在 `research/local/…/rep0/`。
- 用精简版 raw.json 重新分析，得到的 summary.json 和 preview.md 与原先逐字节相同。

## 同轮工程检查（实际执行，日志在 logs/dev012-*.log）

- 构建与全量 CTest：MSVC Release、MSVC ASan、GCC UCRT64 Debug / Release、Clang UCRT64 Debug / Release，各 36/36，构建 0 警告，ASan 没有报告。
- 夹具清理改为有界的 `tests/test_fixture.hpp` 之后，`codec.round_trip`、`archive.store_round_trip`、`archive.store_acceptance`、`seed.archive_integration`、`seed.adaptive_file` 在 GCC Debug 与 Clang Debug 下各重复 50 次（每次 60 s 超时），0 失败、0 超时，没有残留夹具。
- CLion `get_file_problems`（errorsOnly=false）：`test_fixture.hpp`、`codec_tests.cpp`、`store_acceptance_tests.cpp`、`seed_adaptive_tests.cpp`、`seed_archive_tests.cpp` 均 0 ERROR / 0 WARNING，只有 INFO 级风格提示。`store_archive_tests.cpp` 起初两次超时（120 s 与 240 s）；用户把 CLion 切到前台后补查完成：0 ERROR / 0 WARNING，只有 INFO（冗余限定符、const 形参、指定初始值设定项、拼写等，均为既有风格，未改动）。至此 6 个改动的测试文件全部覆盖。
- 没有运行：Fuzz、UBSan、非 Windows 平台；Python 脚本没有做 IDE 诊断。
