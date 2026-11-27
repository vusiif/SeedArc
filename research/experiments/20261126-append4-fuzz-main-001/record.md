# 研究容器读取的模糊测试：hk1 长跑与 DEV-034 后复核

2026-11-26，DEV-032，主Agent（Claude），无委派。状态：执行者报告通过，**未经独立审计**。目标与模糊测试入口见 [append4_fuzz.cpp](../../tools/append4_fuzz.cpp)（严格解析 + 完整解码 + 损坏恢复；每个输入先原样跑一遍，重算全部 CRC 后再跑一遍）。

## 方法

- **目标**：`seedarc_fuzz_append4`，libFuzzer + ASan + UBSan（`-fno-sanitize-recover=undefined`）。模糊测试构建把可接受的帧上限降到 64 MiB，普通构建不变。
- **种子**：[fuzz-corpus/append4](../../tools/fuzz-corpus/append4)，hk1 长跑时有 13 个；DEV-034 后新增 2 个 extreme 种子（见下）。
- **参数**：`-rss_limit_mb=800 -malloc_limit_mb=600 -max_len=16384 -timeout=30`。

## 执行

### 1. hk1 长跑（DEV-034 之前的代码）

- **任务**：`20261126T113814Z-241afb1b`，独占模式，UTC 11:38–21:38，退出码 0。脚本见 [hk1/task.sh](hk1/task.sh)，清单见 [hk1/manifest.json](hk1/manifest.json)。
- **二进制**：us2 上用 Clang 20 构建，SHA 见 [hk1/received.sha256](hk1/received.sha256)（`seedarc_fuzz_append4` 为 `5da9dd18…`），依赖见 [hk1/ldd.txt](hk1/ldd.txt)。
- **结果**（[hk1/fuzz.log.gz](hk1/fuzz.log.gz)）：
  - 36 001 s 内执行 26 206 715 次，平均 727 次/秒；
  - **0 个崩溃 / 超时 / 内存超限产物**，没有 sanitizer 报告；
  - 最终覆盖 cov 2298、ft 11240，语料 1545 个；峰值 RSS 384 MiB，单个输入最慢不到 1 s。
- **最小化语料**：`-merge=1` 后剩 1111 个（[hk1/merge.log](hk1/merge.log)），存为 [hk1/corpus-min.tar.gz](hk1/corpus-min.tar.gz)，SHA256 `0ef55fc3…7b76`。
- **取回与清理**：bundle `c56528c7…` 核对后已清理服务器临时目录。

### 2. DEV-034 之后的复核

hk1 长跑用的是 DEV-034 之前的代码，那时 Seed bit 15 一律非法，所以新合法的 extreme 路径没被跑到。为此做了下面三件事：

- **新增种子**：`r5-recipe-lzma2e-seed.sadx`（`solid/lzma2-9e/g1-r0/frame-16MiB`，Seed 表示，876 B）和 `r5-recipe-lzma2e-explicit.sadx`（`solid/lzma2-1e/g2-r1/frame-whole`，显式表示，977 B）。两者都用 bounded_fixture 的 `first` 树生成，`describe` 报出的配方与写入的一致。
- **回放**：
  - MSVC 回放程序：1111 个最小化输入和 15 个种子全部通过；
  - WSL Clang 21 模糊测试构建（0 警告）：`-runs=0` 回放 1126 个输入，退出码 0，见 [wsl-after-dev034/replay.log](wsl-after-dev034/replay.log)。
- **短跑**：WSL 上以这 1126 个输入为起点跑 1200 s，执行 2 779 620 次，0 个产物，退出码 0，最终 cov 2211、ft 10774，见 [wsl-after-dev034/fuzz.log.gz](wsl-after-dev034/fuzz.log.gz)。

## 结论与限制

- 10 小时、约 2 620 万次执行，没有发现研究容器读取路径上的内存错误、未定义行为、超时或内存超限。
- 两次的覆盖数字（2298 和 2211）来自不同编译器（Clang 20 和 21）和不同代码，不能直接比较。
- 只覆盖研究容器（`append4_study`）的读取与解码，没有覆盖正式归档格式；不能据此宣称没有缺陷。
- DEV-034 后的复核只有 20 分钟，比长跑短得多。
- 最小化语料没有放进 `fuzz-corpus/`，以免回放测试变慢；需要时从本目录的压缩包解出。
