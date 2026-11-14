# 分析脚本

`m3_baseline.py` 使用 Python 标准库生成小型合成数据，运行固定算法、逐文件哈希往返验证并汇总原始指标。用 `--help` 查看用法；每次使用新 run ID，生成数据和大产物保留在 research/local，小型证据保存在 research/experiments。调用者须验证传入 CLI 的 Release 构建来源；内存未测量，合成输入不代表实际性能。

脚本应记录依赖 / 版本、输入格式、命令与输出；从保留的原始指标生成报告，不覆盖原始数据。对缺字段、失败运行及单位明确处理；null / 未测量不当成 0。随机分析固定并记录种子。

`file_scope_table.py <observation.json>` 校验 overlay-r1 的逐行继承 / 清除行为与变化集合，向 stdout 输出对照表；要求选中与未选中文件均存在实际受影响块，避免用空操作证明隔离。只读原始 JSON，不覆盖实验数据；不将拟议计划转换为压缩收益结论。

## B2-r2 文件身份对照

`python research/scripts/file_contract_table.py research/experiments/20261112-file-contract-main-001/observation.json` 验证原始拟议计划的逐字段合并、清除、身份扰动与无变化结果，输出 Markdown 摘要；仅使用 Python 标准库，不执行压缩或性能测试。

## B2-r3 真实压缩基线

`b2_compression.py --tool <Release seedarc_seed_archive.exe> --run-id <new-id> --repetitions 3` 生成磁盘样本并运行8方案的压缩/验证/提取，记录完整归档、逐字节往返、时间、峰值工作集与拟议/实际计划。依赖仅Python标准库和本机研究工具；现有run不覆盖（可预先只登记record.md）。大型产物保留research/local。

`python research/scripts/b2_report.py research/experiments/20261112-b2-compression-main-001/raw.json` 从保存的原始结果复核并生成Markdown表，使用同目录b2_compression.py中的验证/聚合函数，不执行压缩。固定单算法与相同Codec分配的文件覆盖对照分别解释，不自动宣称收益。

## B2-r4 逐块诊断与简单基线

`b2_diagnose.py --tool <Release seedarc_seed_archive.exe> --run-id <new-id> --repetitions 3` 复用 B2-r3 磁盘样本（逐文件哈希核对，源码取 r3 `dataset-source/`），运行 8 个原方案与 B2-file-codec（经工具 `file-plan` 模式）并逐字节往返；导出逐块 `blocks.jsonl` / `blocks.csv`、对账与分组 `summary.json`、损失归因 `attribution.json` 和 `preview.md`。`--analyze-only <run 目录>` 只读 raw.json 重新生成分析，并断言块边界相同、归档对账成立、原方案归档与 B2-r3 哈希一致。仅 Python 标准库；字段语义见 [run 记录](../experiments/20261112-b2-diagnose-main-001/record.md)。

## B2-r5 Adaptive-file 与统一预算对照

`b2_adaptive.py --tool <Release seedarc_seed_archive.exe> --run-id <new-id> --repetitions 3` 复用 r3/r4 样本（哈希核对），运行 8 个 r4 原方案（P0-A 复现，要求归档 SHA256 与 r4 相同）、`oracle-file`（须等于 r4 B2-file-codec）与 `adaptive-file` 四档预算（0、256 KiB、1 MiB、无界），全部逐字节往返；输出 `summary.json`（P0-A 核对、预登记假设、逐组成本）、`preview.md` 与 `size-vs-time.svg`。`--analyze-only <run 目录>` 只读 raw.json 重新生成。导入同目录 `b2_diagnose.py` 的样本与工具函数；仅 Python 标准库。选择器参数在 [seed-adaptive-file-r5](../ideas/seed-adaptive-file-r5.md) 预先登记，不在开发样本上调参。

## B2-r5 P1 独立语料与评估

`b2_corpus.py [--corpus-id b2r5-corpus-r1]` 从本机已安装的软件和系统文件构建 10 类数据池，在压缩之前按相对路径哈希分成 dev / test。内容复制到被忽略的 `research/local/<corpus>/`；元数据（相对路径、字节、SHA256、许可证、块数）写入 `research/datasets/`。不下载任何数据，清单不写绝对路径。

`b2_independent.py --tool <Release seedarc_seed_archive.exe> --run-id <new-id> --repetitions 5` 只评估 test 划分：Fixed Zstd-3、Fixed LZMA2-6、冻结的 Adaptive-file-r1 三档预算、Oracle-file、B-r1 seed0（消融 G4）。运行前先按清单核对 test 划分；方案顺序每次轮换；测量前做热缓存。输出 summary.json（均值、中位数、分位数、逐文件节省分布、每 KiB 的额外 CPU）与 preview.md。`--analyze-only` 只读 raw.json 与 per_file.csv 重新生成分析。依赖同目录的 `b2_adaptive.py` / `b2_diagnose.py`；仅使用 Python 标准库。

## DEV-013 分组语料与共享字典

`b2_corpus_r2.py` 构建 b2r5-corpus-r2：按项目分组，共享内容的分组合并成连通分量，分量整体划入 dev 或 test；空文件和生产归档拒绝的路径会被排除。`--check` 根据已提交的清单验证 test 与 dev 的内容哈希交集为 0。

`b2_dictionary.py --phase dev|test` 调用研究工具 `seedarc_dict_study`（研究容器）和 `seedarc_seed_archive`（生产参照），完成打包、内存校验、冷启动单文件提取，并给出分位数与逐文件分布。dev 阶段写出 `selection.json`；test 阶段必须读入它。test 只能运行一次，再次运行需要 `--rerun-reason`。只依赖 Python 标准库。
