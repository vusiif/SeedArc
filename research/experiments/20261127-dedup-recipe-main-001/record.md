# 配方整文件去重的实现与核对（ADR-0008）

2026-11-27，主 Agent（Claude），无委派。状态：执行者报告，**未经独立审计**。

实现 [ADR-0008](../../../docs/decisions/0008-recipe-whole-file-dedup.md)（用户 2026-11-27 接受）。研究容器修订 5 的配方生成可以带 `/dedup`：同代、同组内逐字节相同的非空文件写成别名条目，帧边界保持不变。

## 问题

1. 实现出来的归档，帧字节是否与 DEV-036 用 `dedup_same_cut` 预测的完全相同？
2. 完整归档能省多少？元数据有没有额外开销？

## 方法

- **代码**：HEAD `dd64aac` 加未提交改动，涉及：
  - `solid_plan.hpp`：Seed bit 16、显式第二字 bit 17、文本 `/dedup`；
  - `append4_study.cpp`：`encode_pending` 写别名；`check_recipe_frames` 核对别名；
  - `recipe_commands.hpp`：`describe` 报出别名数和别名字节；
  - 测试与 fuzz 样本。
- **工具**：MSVC Release，SHA256 `faeb0c19…`（复制到临时目录后运行）。
- **数据**：dev 数据，未碰留出集。
  - [append-r1](../../datasets/append-r1.md) dev；
  - [dup-local-dev-r1](../../datasets/dup-local-dev-r1.md) 的 cargo、rustup、全部。
- **配方**：`solid/<zstd-19|lzma2-9e>/g1-r0/frame-32MiB`，每组分别打包不去重和带 `/dedup` 两份，共 16 个归档。
- **流程**：每个归档都跑 `verify`（逐字节对比原树）和 `describe`。命令、退出码和归档 SHA256 见 [local/commands.txt](local/commands.txt)；归档用完即删，原始 JSON 在 [local/](local/)。

## 结果

| 树 | 配方 | 不去重：归档 / 帧 | 去重：归档 / 帧 | 别名 | 完整归档变化 | 帧字节与 DEV-036 一致 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| append-r1 dev | Zstd-19 | 39 803 077 / 8 | 38 237 426 / 8 | 19 | **−3.93%** | 是 |
| cargo | Zstd-19 | 24 086 211 / 9 | 22 741 929 / 9 | 3 025 | **−5.58%** | 是 |
| rustup | Zstd-19 | 57 730 667 / 9 | 57 664 604 / 9 | 16 | −0.11% | 是 |
| 全部 | Zstd-19 | 83 689 536 / 18 | 82 276 227 / 18 | 3 218 | **−1.69%** | 是 |
| append-r1 dev | LZMA2-9e | 37 503 831 / 8 | 36 101 964 / 8 | 19 | **−3.74%** | 是 |
| cargo | LZMA2-9e | 23 171 178 / 9 | 21 852 175 / 9 | 3 025 | **−5.69%** | 是 |
| rustup | LZMA2-9e | 51 240 508 / 9 | 51 181 600 / 9 | 16 | −0.11% | 是 |
| 全部 | LZMA2-9e | 76 235 571 / 18 | 74 854 271 / 18 | 3 218 | **−1.81%** | 是 |

- **预测完全兑现**：
  - 去重归档的帧字节等于 DEV-036 的 `dedup_same_cut`，不去重归档的帧字节等于 `baseline`，8 组都逐字节相同；
  - 别名数等于 DEV-036 数出的重复文件数（例如"全部"是 3 218）；
  - 帧数不变。
- **没有元数据开销**：trailer 大小与不去重时完全相同（"全部" LZMA2-9e 都是 1 289 760 B）。别名只是让条目的四个字段指向原件，条目长度不变。
- 16 个归档都通过 `verify`。

## 测试

- **`research.solid_recipe`**：答案已知的树 `p/{a,a2,b,c,d,e0,e1}` 加 `q/a`。
  - 正向用例：
    - Seed 和显式两种表示：3 个别名、21 MiB、3 帧，两种表示的载荷相同；Seed 设了 bit 16，hex 往返相同；
    - 帧字节等于 `dup-probe` 的 `dedup_same_cut`，并且比不去重小；
    - g2-r0 下 `q/` 单独成组，只有 2 个别名；整帧 LZMA2-6e 也是 3 个别名；
    - 别名文件、跨帧别名、空文件都能单独提取；`--jobs 3` 字节相同；
    - 追加的新一代不跨代去重；合并后重新判定得到 4 个别名，与直接打包合并树的字节相同。
  - 拒绝用例，都要求 `verify` 和 `describe` 拒绝、`extract` 不写出文件：
    - 去掉 dedup 位但别名还在；
    - 别名指向另一个同大小文件的位置（CRC 不同）；
    - 别名指向更后面的文件；
    - 别名跨组；
    - 别名跨代；
    - 文本拼写：重复的 `/dedup`、大写、末尾斜杠、放错位置、用短横线；
    - Seed bit 17、显式第二字 bit 18。
  - 原来把 Seed bit 16 / 显式 bit 17 当保留位的拒绝用例（包括 Seed `0x12500`），现在这一位是合法的 dedup 位，改为测 bit 17 / bit 18（Seed `0x22500`）。
- **Fuzz 样本**：
  - `r5-recipe-dedup-seed.sadx`：`solid/zstd-9/g1-r0/frame-16MiB/dedup`，898 B，2 个别名；
  - `r5-recipe-dedup-explicit.sadx`：`solid/lzma2-1e/g2-r1/frame-whole/dedup`，1 045 B，2 个别名；
  - 两者都用 bounded_fixture 的 `first` 树，加上一个文件的两份副本生成。
- **构建**：MSVC Release 0 警告，CTest 50/50。WSL 结果见下节。

## WSL 与 fuzz

- **快照**：工作树快照 `e146a15`（临时 ref，用完已删）。
- **测试**：WSL GCC 14 Release CTest 51/51；Clang ASan+UBSan 22/22，没有 Sanitizer 报告；两个构建都是 0 警告。
- **跨工具链**：3 个固定小包，GCC 与 MSVC 的 SHA256 相同：
  - 测试树 `solid/zstd-9/g1-r0/frame-16MiB/dedup`：`83fe71aa…`；
  - 两个 fuzz 样本：Seed 表示 `aee6518a…`、显式表示 `788678d7…`。
- **Fuzz**：libFuzzer + ASan，目标 SHA 前缀 `0bab18c2…`，构建 0 警告。
  - 语料 1 128 个：DEV-032 合并后的 1 111 个，加仓库里的 17 个种子（含 2 个新的 dedup 样本）。回放退出码 0。
  - 运行 1 200 s，共 2 876 991 次执行，0 个 artifact；cov 从 2223 升到 2232，峰值 RSS 470 MB。

## 结论与限制

- ADR-0008 的实现与测量预测逐字节一致。完整归档在有跨帧副本的数据上省 3.7–5.7%，三个池合计省 1.7–1.8%，没有副本时为 0，没有元数据开销。
- **限制**：
  - 只在 dev 数据上核对，没有在留出集上评估；
  - 去重的判定代价没有单独计时：要对每个文件算 CRC32C，同桶的再逐字节比较；
  - 损坏影响：跨帧副本依赖原件所在的帧（ADR-0008 §4）；修订 5 没有恢复功能；
  - CLion 诊断未运行（MCP 连接失败）。
