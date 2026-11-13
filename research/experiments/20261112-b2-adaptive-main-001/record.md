# B2-r5 P0：可复现基线、Adaptive-file 选择器与统一预算对照

日期：2026-11-12（America/Los_Angeles），运行 07:41:02–07:41:26。任务 DEV-010，主 Agent（由 B2 协调会话委派）。输入计划：[B2-r4 评估与 B2-r5 实施计划](../../../docs/review_results/SeedArc_B2-r4评估与B2-r5实施计划.md) 的 P0-A / P0-B / P0-C。运行前登记的规则与假设：[seed-adaptive-file-r5](../../ideas/seed-adaptive-file-r5.md)。

**结论**：在这 5 组开发样本上，Adaptive-file（`adaptive-file-r1`）各预算档都**不比 Fixed Zstd-3 大**，有足够预算时（≥256 KiB）五组合计比 Zstd-3 小 1,203 B，只比 Oracle-file 大 2 B。全部收益都来自 source 中的一个文件 `store_archive.cpp`（改选 LZMA2-6）；其余四组没有空间收益，却仍要付出试编码成本。合计墙钟：Zstd-3 60 ms，Adaptive 256 KiB 72 ms，1 MiB / 无界 约 130 ms，Oracle 198 ms，Fixed LZMA2-6 191 ms（比 Zstd-3 小 320 B）。样本太少，不能推出普遍收益或耗时结论；按计划没有用这批样本调参数。

## P0-A：基线复现

- 输入：沿用 B2-r3 / r4 的 5 组磁盘样本，逐文件 SHA256 与 r3 `datasets.json` 一致（源码取自 r3 `dataset-source/`）。
- 复现结果：8 个 r4 原方案（store、zstd3、zstd6、lzma6、b-base、b-global、b-file、b-cleared），加上 `oracle-file`（对照 r4 的 `b2-file-codec`），共 45 组归档的 SHA256 **与 B2-r4 完全相同**（见 `summary.json` → `p0a_identical_to_r4`）。这说明新加的进程内 Oracle 和 r4 用多次压缩外部选出的结果逐字节一致，研究工具输出扩展也没有改变归档。
- B0（b-global）仅作为诊断回归保留，不参与下面的结论。
- B2-r4 的证据目录原样保留，未改动。本 run 冻结了以下内容：
  - 源码：`source/`、`source-sha256.json`、`tracked-source.patch`、`git-status.txt`；基线提交 `f3deb4a`，工作区 dirty。
  - 构建：`msvc-vs2026-release`，Release，`/O2 /Ob2 /DNDEBUG`，VS 18 2026 生成器，vcpkg triplet `x64-windows-static-md`（见 `config.json` → build）。
  - 依赖：`vcpkg.json` SHA256 `d2b23b81…6641`；zstd 1.5.7、liblzma 5.8.4 由 overrides 钉住。
  - 工具二进制 SHA256：`3021923d…97d2`，副本在 `research/local/20261112-b2-adaptive-main-001/`。
  - 数据集：`datasets.json`。

## P0-B：Adaptive-file 选择器

实现在 [core/seed/adaptive_file.cpp](../../../core/seed/adaptive_file.cpp)（研究库 `seedarc_seed_study`，不属于生产 core）。

- **接入方式**：它运行在 core 已有的 `PlanBuilder` 回调里，只输出每块的 `BlockEncoding{Codec, config}`。编码、解码、索引和提交路径都没有改；没有新增归档字段或块段字段；解码不会重新执行选择。
- **读取抽样块**：先用 core 的 `InputTree` 再枚举一次输入，再按清单路径打开文件（不跟随重解析点）；文件大小与清单不一致就报错。真正写归档时 core 会重新读取并检查每个文件，所以选择之后文件若被改动，只会让决策过时，不会写出错误的归档。
- **抽样与预算规则**：完全按登记时的 r1 执行，没有改动：
  - 默认 Zstd-3；
  - 小于 16 KiB 的文件不试编码；
  - 抽首、中、尾三块；
  - 先试 Zstd-3 与 LZMA2-6，Zstd-3 至少省 2 % 时才试 Zstd-6；
  - 候选要同时满足“比 Zstd-3 抽样省 ≥ 2 %”和“外推到整个文件省 ≥ 64 B”才会切换；
  - 预算按文件整体预留，余额不够就跳过该文件，不会只做一半；
  - 试编码失败时回退 Zstd-3，归档照常写出。
- **决策原因**：`empty_file`、`below_min_size`、`budget_exhausted`、`no_gain`、`insufficient_gain`、`lzma2_6_saves`、`zstd_6_saves`、`trial_failed`；另有 `notes`（如 `zstd6_skipped_budget`）。
- **研究证据**：只写在工具输出 JSON 的 `selector` 字段里（`raw.json`），不写进归档。包含 `selector_version`、每个文件的 `sampled_blocks`、每次试编码的 codec、等级、输入和输出字节、读取字节、试编码墙钟与 CPU、`budget_limit`、`budget_used`、`decision_reason`。
- **元数据**：`metadata_bytes = 0`。原因是选择结果已经完全体现在既有索引里逐块记录的 Codec / config 中。这一点与 B 方案不同：B 的 Seed / 覆盖字段是“尚未序列化”的缺项。
- **新增 CTest `seed.adaptive_file`**，覆盖：
  - 确定性；
  - 空文件、小文件、无收益文件的回退；
  - 首 / 中 / 尾抽样位置；
  - 走到 LZMA2-6 切换分支；
  - 用测试里独立重写的规则，从记录的试编码数据推导决策并核对；
  - 0 预算时与公共固定 Zstd-3 入口逐字节相同；
  - 部分预算时不超支、后续文件回退、`zstd6_skipped_budget`；
  - Oracle 对每个字节试编码三次；
  - 往返逐字节一致；
  - 非法参数与取消时不发布归档。

## P0-C：统一预算对照（完整表见 [preview.md](preview.md)）

| 方案 | 五组归档合计 B | 相对 Zstd-3 | 候选数 | 试编码输入 B | 决策墙钟 ms | 合计墙钟 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed Zstd-3 | 1,192,886 | 0 | 0 | 0 | 0 | 60.3 |
| Fixed LZMA2-6 | 1,192,566 | −320 | 0 | 0 | 0 | 190.7 |
| Adaptive-file @ 0 | 1,192,886 | 0 | 0 | 0 | 0.9 | 59.3 |
| Adaptive-file @ 256 KiB | 1,191,683 | −1,203 | 3 | 102,108 | 6.2 | 71.9 |
| Adaptive-file @ 1 MiB | 1,191,683 | −1,203 | 39 | 2,330,614 | 64.7 | 131.6 |
| Adaptive-file @ 无界 | 1,191,683 | −1,203 | 39 | 2,330,614 | 63.0 | 128.3 |
| Oracle-file（参考上限） | 1,191,681 | −1,205 | 108 | 6,008,163 | 132.6 | 197.7 |

- **各数据集的情况**：
  - source：Adaptive 8,300 B，Oracle 8,298 B，Zstd-3 9,503 B，LZMA2-6 8,304 B。差别在 `checked_arithmetic.cpp`（770 B）：它低于 16 KiB，Adaptive 不试编码，保持 Zstd-3；Oracle 改用 Zstd-6，省 2 B。
  - text、mixed：Adaptive 试过之后判定 `insufficient_gain`（Zstd-6 与 Zstd-3 相同，LZMA2-6 更大），保持 Zstd-3。
  - binary、compressed：判定 `no_gain`。
- **256 KiB 档为什么便宜**：text、mixed、binary、compressed 里需要抽样的文件都要 384 KiB 预算（3 块 × 64 KiB × 2 次试编码），超出这一档，于是记 `budget_exhausted`，直接用 Zstd-3；只有 source 的 34 KiB 文件得到了预算。这个结果取决于清单顺序和这批样本的文件大小，**不能当作 256 KiB 普遍最划算的证据**。
- **决策成本**：1 MiB 档与无界档的结果完全相同（这批样本用不到 1 MiB 以上的预算），比 Zstd-3 多出约 68 ms 合计墙钟，而空间收益只有 source 那 1,203 B。不划算的试编码主要花在 binary 和 compressed 上（每组决策约 12–21 ms，没有任何收益）。
- **计量口径**：
  - 合计墙钟与 CPU：在工具内包住 compress 调用来测，包含决策；最终编码时间 = 合计 − 决策。
  - Python 侧的进程墙钟也保存在 `raw.json`，含进程启动与 JSON 输出。
  - CPU 用 GetProcessTimes / GetThreadTimes，Windows 粒度约 15.6 ms，所以表里的 CPU 值是量化后的数，只能看量级。
  - 解压时间同时有工具内 extract 调用墙钟 / CPU 和进程墙钟。
  - 峰值内存是每个进程的 PeakWorkingSetSize，包含工具与报告。
  - 每组 3 次取中位数，没有预热，OS 缓存不受控，运行顺序每次轮转。
- **Oracle 与 r4 的成本口径差异**：Oracle 的试编码在进程内完成，成本完整计入，没有剔除。r4 是用 3 次完整外部压缩来估算 Oracle 成本，两种口径不同，但最终归档逐字节相同。
- **归档大小与时间关系图**：[size-vs-time.svg](size-vs-time.svg)。

## 预先登记假设

- H1（任何预算档都不比 Zstd-3 大）：5 组 × 4 档全部成立。
- H2（无界档与 Oracle 的差距不超过 Oracle 相对 Zstd-3 节省量的一半）：5 组都成立。但只有 source 是有意义的检验（2 B 对比 1,205 B）；其余四组 Oracle 与 Zstd-3 一样大，条件自动成立。

## 限制与下一步

- 5 组样本都是开发样本，没有独立评估集；不能宣称 Adaptive 在真实数据上划算。
- 阈值（16 KiB、2 %、64 B、三块抽样）是运行前拍定的，没有在这批数据上调过；按计划也不应该在这批数据上调。
- 下一步是 P1：引入独立授权语料，预先划分开发集与评估集，在评估集上一次性检验 r1 规则。
- 未运行：GCC 构建、Fuzz、UBSan、非 Windows 平台。

## 复现

```powershell
$env:VCPKG_ROOT='C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg'
cmake --build build/msvc-vs2026-release --config Release
ctest --test-dir build/msvc-vs2026-release -C Release --output-on-failure
python research/scripts/b2_adaptive.py --tool build/msvc-vs2026-release/research/tools/Release/seedarc_seed_archive.exe --run-id 20261112-b2-adaptive-main-001 --repetitions 3
python research/scripts/b2_adaptive.py --analyze-only research/experiments/20261112-b2-adaptive-main-001
```

- 脚本 SHA256 `7157c7e3…c967`；本目录有它和它依赖的 `b2_diagnose.py` 的副本。
- `raw.json` 记录 195 次操作（13 个方案 × 5 组 × 3 次），含完整工具输出。
- 操作日志共 585 份，与构建 / 测试日志一起放在 `logs/`。
- 归档与解压产物约 127 MB，放在被忽略的 `research/local/` 下。

## 构建与检查（实际执行）

| 检查 | 结果 | 日志（logs/） |
| --- | --- | --- |
| MSVC Release 构建 + 全量 CTest（含新 `seed.adaptive_file`） | 0 警告；36/36（最终版 48.99 s） | b2-r5-release-*.log、b2-r5-final-release-*.log |
| MSVC ASan Release 构建 + 全量 CTest | 36/36，62.06 s，无 ASan 报告；最终修正后定向重建 `seed.*` 5/5 | b2-r5-asan-*.log、b2-r5-final-asan-*.log |
| Clang UCRT64 Debug 定向（工具 + 新测试） | 首次构建报 1 条有效警告（`-Wmissing-designated-field-initializers`，`FileDecision` 指定初始化缺字段），修正后 0 警告；`seed.adaptive_file` 与 `seed.archive_integration` 通过 | b2-r5-clang-*.log、b2-r5-final-clang-*.log |
| 实验 | 195/195 往返；P0-A 45 组同哈希；每组三次的决策与计划一致 | b2-r5-adaptive-console.log、raw.json |
| 最终二进制复核 | 上面那处初始化修正不改变行为，但工具二进制变了（最终 SHA256 `7a37a5d6…b7ff`）；用它重跑第 0 次重复中 7 个 P0-C 方案 × 5 组，归档哈希、决策、逐次试编码输出全部与本 run 相同 | final-tool-recheck.json、final_tool_recheck.py |
| clang-format `--dry-run --Werror` | 4 个改动 C++ 文件通过 | — |
| CLion `get_file_problems`（errorsOnly=false） | `adaptive_file.cpp` / `.hpp`、`seed_archive.cpp`、`seed_adaptive_tests.cpp`、`core/CMakeLists.txt`、`tests/CMakeLists.txt`：0 ERROR。测试文件 2 条 WARN（局部 constexpr 可移入内层作用域、变量可设为 constexpr）已修正并复查消失；其余为 INFO 风格提示（指定初始值设定项、const 形参、冗余 static_cast、拼写），与既有代码一致，未改 | — |

**既有测试的偶发失败（不是本轮引入，也没有改动）**：`seed.archive_integration`（B2-r3 的测试）在一次 `-R seed` 运行中失败。单独把它重复跑 15 次，失败 1 次，断言是 “failed planned writes leave no temporary artifacts”：失败写入之后立刻数目录项，偶尔会多出一项，但事后检查目录里并没有残留文件。推测是 Windows 延迟删除（句柄尚未关闭）造成的时序问题。这次失败留下的测试夹具已确认没有重解析点，并已清理。需要单独排查。

未运行：GCC 构建、Fuzz、UBSan、非 Windows 平台。CLion 只检查了本轮改动的文件；Python 脚本与 Markdown 未做 IDE 诊断。
