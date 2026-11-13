# B2-r4 P0：逐块诊断与简单 Codec 分配基线

日期：2026-11-12（America/Los_Angeles），运行 06:49:23–06:49:41。任务 DEV-009，主 Agent（由 B2 协调会话委派）。输入计划：[B2-r3 首轮评估与 B2-r4 优化计划](../../../docs/review_results/SeedArc_B2-r3首轮评估与B2-r4优化计划.md) 的 P0-A / P0-B。思路与假设见 [seed-codec-allocation-r4](../../ideas/seed-codec-allocation-r4.md)。

**结论**：text / mixed 上 B0 与固定 Zstd-3 的差距 **99.78 % 来自 B-r1 与内容无关地把块计划为 Store**（不尝试压缩，原样写入）；其余 0.22 % 来自 LZMA2-6 在高度重复的 64 KiB 文本块上比 Zstd 多 72 B/块。Zstd 4 vs 3、头部、索引的贡献均为 0。简单的 B1-fixed-zstd3 与 B2-file-codec 都完全回收这部分损失。本轮未改算法、未扩大 Seed 搜索、未冻结格式。

## 问题与预先登记的方法

问题：B2-r3 中 `text` 上 `b-global` 66,538 B 对 `zstd3` 944 B，`mixed` 上大 66,903 B（+16.94 %），损失来自哪些块、哪类决定？简单分配能否回收？

方法：复用 B2-r3 的 5 组磁盘输入（逐文件 SHA256 与 r3 `datasets.json` 一致，源码样本取自 r3 `dataset-source/`），同一 64 KiB 固定分块、单线程、同一归档计量口径。10 个方案 × 5 数据 × 3 次，每次 compress → verify → extract 并逐字节比较：

| 方案 | 规则 | 计划来源 `plan_source` |
| --- | --- | --- |
| store / zstd3 / zstd6 / lzma6 | 固定单 Codec，压缩块不严格变小时回退 Store | `fixed` |
| b-base / **b-global（B0-current）** / b-file / b-cleared | B2-r3 原样：B-r1 codec_seed=0，Zstd 等级 3 / 4 / 目标文件 6 / 4，LZMA2 preset 6 | `base` / `global` / `file_override` |
| **zstd3（B1-fixed-zstd3）** | 全部块 Zstd-3，无收益回退 Store | `fixed` |
| b2-file-codec-c | 每文件在 zstd3 与 lzma6 中取总存储字节较小者（平手取 zstd3） | `file_codec` |
| **b2-file-codec（B2-file-codec）** | 上一步之后，若该文件为 Zstd 且 zstd6 更小，则文件级等级覆盖 3→6 | `file_codec` / `file_override` |

B2-file-codec 是**编码侧 oracle**：选择依据来自同一 run、同一块边界下第 0 次重复的 zstd3 / zstd6 / lzma6 实测逐块数据（每文件 3 个试编码配置，成本约等于额外 3 次完整压缩），随后用研究工具 `file-plan` 模式把具体计划交给生产 core 真实压缩。它不是 Seed 编码；归档索引本就逐块保存实际 Codec / config，解码无需额外元数据。Store 不单列候选，由 core 的逐块回退隐式覆盖。

## 字段语义（`blocks.jsonl` / `blocks.csv`）

- `dataset_id`：数据集名；`mode` / `strategy`：方案与 P0-B 标签。
- `file_key`：归档内规范化 UTF-8 相对路径（`/` 分隔，Windows NFC 路径规则校验），也是 B2 文件覆盖的匹配键。`file_id` 是本次清单条目编号（含目录），**不是稳定身份**。
- `block_index`：该块在本归档块表（索引 `blocks` 数组）中的位置，即实际写出顺序。
- `block_id`：B-r1 用来计算 Codec 的逻辑全局块 ID。本轮恰好等于本次规范块序号 0..N−1（与 `block_index` 数值相同），但语义不同：插入文件会让后续 `block_id` 改变，进而改变 B-r1 分配；`block_index` 只描述布局。
- `file_block_index`：文件内第几块。
- `planned_*` / `actual_*`：计划与归档索引中的实际 Codec / 等级（Store 等级为 0）。
- `candidate_status`：`not_attempted`（计划 Store，不尝试压缩）/ `accepted`（压缩块严格小于原文）/ `rejected_not_smaller`。
- `candidate_encoded_bytes`：仅 `accepted` 时给出（= `stored_bytes`）。被拒候选的确切长度 core 不计算（编码器输出缓冲为 raw−1，放不下即判无收益），记 null，只知道 ≥ raw；不为诊断改动 codec 边界。
- `stored_bytes`：最终写入数据区的字节；`fallback_to_store`：计划非 Store 但实际 Store；`fallback_reason`：本轮只出现 `no_space_gain`；`store_cause`：`planned_store` / `fallback_no_space_gain` / null。

## 完整归档对账

每个方案：`archive_bytes = 头部 96 + data_bytes + index_bytes`，且 `data_bytes = Σ stored_bytes`，全部 50 组成立（`summary.json` → `accounting.reconciles`）。同一数据集内所有方案的索引字节相同（text 332、mixed 976、source 294、binary 334、compressed 381），块边界（`file_key`、`file_block_index`、`block_id`、`raw_bytes`）逐块相同，故差距全部落在数据区。**Seed / 覆盖元数据：未序列化（header strategy_version=0、seed_length=0），记为缺项而非 0**；B 方案将来序列化后还须加上该成本，B1 / B2-file-codec 不需要。

## 结果（完整归档字节）

| 数据 | B0-current | B1-fixed-zstd3 | zstd6 | lzma6 | b-file | B2-file-codec（仅 Codec） | B2-file-codec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| text | 66,538 | 944 | 944 | 1,379 | 66,538 | 944 | 944 |
| mixed | 461,862 | 394,959 | 394,959 | 395,403 | 461,862 | 394,959 | 394,959 |
| source | 9,560 | 9,503 | 8,835 | 8,304 | 8,893 | 8,300 | 8,298 |
| binary | 393,646 | 393,646 | 393,646 | 393,646 | 393,646 | 393,646 | 393,646 |
| compressed | 393,834 | 393,834 | 393,834 | 393,834 | 393,834 | 393,834 | 393,834 |

八个原方案的 40 组归档与 B2-r3 run 的 SHA256 逐一相同（`summary.json` → `archives_identical_to_b2_r3`），说明研究工具输出扩展未改变归档。

### text：+65,594 B 的来源（6 块 × 65,536 B，单文件）

| 类别 | 块 | 每块 B0 → B1 | 合计 | 占比 |
| --- | --- | --- | ---: | ---: |
| B-r1 计划 Store（未尝试压缩） | 2 | 65,536 → 86 | +65,450 | 99.78 % |
| LZMA2-6 vs Zstd-3 | 0, 4 | 158 → 86 | +144 | 0.22 % |
| Zstd-4 vs Zstd-3 | 1, 3, 5 | 86 → 86 | 0 | 0 |
| 头部 + 索引 | — | 428 → 428 | 0 | 0 |

### mixed：+66,903 B（+16.94 %）的来源（14 块）

| 类别 | 块 | 合计 | 说明 |
| --- | --- | ---: | --- |
| 计划 Store：文本 | 2 | +65,450 | 同 text |
| 计划 Store：`sub/packed.gz`（1,440 B 的文本 gzip） | 12 | +1,309 | Zstd-3 得 131 B |
| 计划 Store：噪声 / 24 B 小文件 | 7, 10, 13 | 0 | 固定 Zstd 同样回退 Store |
| LZMA2-6 vs Zstd-3（文本） | 0, 4 | +144 | |
| 回退 Store（噪声，无空间收益） | 6, 8, 9, 11 | 0 | B1 同样回退 |
| Zstd-4 vs Zstd-3 | 1, 3, 5 | 0 | |
| 合计 | | +66,903 | 计划 Store 占 66,759（99.78 %） |

### source 与其它

source：B0 比 B1 多 57 B（计划 Store 的 `version.cpp` 230 → 174 B 占 56 B，Zstd-4 多 1 B）。B2-file-codec 为 `store_archive.cpp` 选 lzma6、`checked_arithmetic.cpp` 覆盖为 zstd6（省 2 B）、`version.cpp` 选 zstd3，得 8,298 B，比 lzma6 少 6 B、比 b-file 少 595 B。这是单样本、3 配置 oracle 的结果，差值很小，不构成普遍收益结论；文件级等级覆盖在本轮只贡献 2 B。binary / compressed：所有方案相同，计划 Store 与回退块结果一致，无损失。

## 解释边界

- 测得的事实：哪些块被计划为 Store、各块实际字节。原因“B-r1 的 `hash mod 3` 分配与内容无关，约 1/3 块直接 Store”来自 [strategy.cpp](../../../core/seed/strategy.cpp) 的实现与逐块数据；LZMA2 每块多 72 B 的内部原因（流格式开销等）**未单独验证**。
- text 是 78 B 行重复 6000 次的极端样本，Zstd-3 每块 86 B；不能推广到真实文本。样本仍为 B2-r3 的自制小样本与本项目源码，无独立语料、无调参/评估划分。
- 时间与峰值工作集照旧记录在 raw.json / summary.json（进程墙钟，含启动、计划、自校验与 JSON 输出；3 次、无预热、缓存不受控），本轮不据此下任何耗时结论。
- B2-file-codec 的选择用到了实测压缩结果，属编码侧搜索；与 B1 比较时须记得它多花约 3 次完整压缩，本轮未计入预算对比。

## 复现

```powershell
$env:VCPKG_ROOT='C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg'
cmake --build build/msvc-vs2026-release --config Release          # 已有 configure，SEEDARC_BUILD_RESEARCH_TOOLS=ON
ctest --test-dir build/msvc-vs2026-release -C Release --output-on-failure
python research/scripts/b2_diagnose.py --tool build/msvc-vs2026-release/research/tools/Release/seedarc_seed_archive.exe --run-id 20261112-b2-diagnose-main-001 --repetitions 3
python research/scripts/b2_diagnose.py --analyze-only research/experiments/20261112-b2-diagnose-main-001   # 只读 raw.json 重新生成分析
```

代码：基线提交 `f3deb4a`，工作区 dirty（含其他会话未提交的 B2-r1/r2/r3 与本轮改动）；`tracked-source.patch` 为受跟踪文件差异，`git-status.txt` 为运行后状态，`source/` 保存相关未跟踪 / 修改源码，`source-sha256.json` 绑定哈希。工具 SHA256 `a3fce975…88f4`（副本在 `research/local/20261112-b2-diagnose-main-001/`）；工具 `--version` 文字仍为 “B2-r3 study”，库版本 zstd 1.5.7、liblzma 5.8.4（vcpkg 钉住）。环境：Windows 10 19045、AMD64 Family 25 Model 116、16 逻辑核、Python 3.15.0、MSVC Release（msvc-vs2026-release）。

脚本：运行时版本 SHA256 `c30b5235…2e3f`（config.json）。运行后为分析部分加了两处：CSV 布尔改为小写 `true/false`，并新增与 B2-r3 归档哈希的逐组比较；随后用 `--analyze-only` 重新生成全部派生文件。本目录 `b2_diagnose.py` 为最终版本（SHA256 `5b87385f…f536`），执行部分未变。

## 文件

`raw.json`（150 次操作，含完整工具输出）、`blocks.jsonl` / `blocks.csv`（360 行逐块数据，每数据集×方案取第 0 次；三次重复归档哈希相同）、`summary.json`（对账、按实际 / 计划 Codec、文件、回退原因、计划来源分组）、`attribution.json`（B0 对 B1、B0 对 B2-file-codec 的逐类字节差，脚本断言其和等于归档差）、`file-codec-trials.json`、`plans/`（B2-file-codec 实际计划）、`preview.md`（生成表）、`cli-smoke.json`（`file-plan` 负面输入 11 项）、`datasets.json`、`config.json`、`logs/`（450 份操作日志与构建 / 测试日志）。归档与解压产物 100 MB 留在被忽略的 `research/local/`。

## 构建与检查（实际执行）

| 检查 | 结果 | 日志（logs/） |
| --- | --- | --- |
| MSVC Release 构建 + 全量 CTest | 0 警告；35/35，50.92 s | b2-r4-release-{build,test}.log |
| MSVC ASan Release 构建 + 全量 CTest | 35/35，59.31 s，无 ASan 报告 | b2-r4-asan-{build,test}.log |
| Clang UCRT64 Debug 定向构建 `seedarc_seed_archive` | 成功，0 警告 | b2-r4-clang-build.log |
| 实验运行 | 150/150 往返；对账 50/50；块边界一致；40 组原方案归档与 B2-r3 同哈希 | b2-r4-diagnose-console.log、raw.json |
| `file-plan` 负面输入 | 11 项均按预期拒绝，失败后无归档 | cli-smoke.json |
| clang-format `--dry-run --Werror` | 首次检查 2 处违规（只涉及空白）；`clang-format -i` 后通过 | — |
| 格式化后复核 | 重新构建 Release 工具（0 警告）；mixed/b-global 与 source/B2-file-codec 重新压缩，归档与本 run 逐字节相同 | b2-r4-release-fmt-build.log |
| CLion `get_file_problems`（errorsOnly=false） | `research/tools/seed_archive.cpp`：0 ERROR / 0 WARNING；38 条 INFO（指定初始值设定项、const 形参、冗余 static_cast、拼写），与既有代码风格一致，未改动 | — |

本轮运行的 Release 工具二进制编译自格式化**之前**的源码（SHA256 见 config.json 的 tool_sha256）。`source/research/tools/seed_archive.cpp` 是格式化之后的版本（`0b572bbd…70d2`），两者只差空白；上面的复核表明行为没有变化。CLion 只检查了本轮改动的 C++ 文件；Python 脚本与 Markdown 没有做 IDE 诊断。未运行：GCC 构建、Fuzz、UBSan、非 Windows 平台。
