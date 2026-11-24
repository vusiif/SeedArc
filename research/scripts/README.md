# 分析脚本

## DEV-017-r2 独立历史验收与成本审查

`append_history_r2.py --tool <v4 工具> --input <输入根> --manifest <dev TSV> --output <新目录>` 验收独立的 A / B-new / C / C-coord / 显式 / 固实历史与冷启动对照，最多接收 8 MiB。它只验证流程，**不能作为大规模性能运行器**。每批独立 verify，记录 parent / plan / artifact 哈希，拒绝复用已有输出目录。

`check_append_history_r2.py --tool <v4 工具> --work <新目录>` 生成含中文路径的四批夹具，运行两遍并核对历史与字节确定性。`append_space_audit.py --raw <pilot raw.jsonl> --output <新 JSON>` 对当前可分离目标作成本上界推导，读取旧记录仅作描述性观察，不执行新压缩或给出 Seed 收益结论。目标与未满足的主实验条件见 [r2 方案](../ideas/seed-append-main-r2.md)。

## DEV-017 印度固定主机 dev 首轮

本轮已因定位复核主动中止，以下脚本保留为流程预演的复现材料，不应当作主实验重新启动。

`append_india_task.sh` 通过 `in-long-task` 启动；`append_india_bootstrap.py` 仅在任务目录解包工具链，使用仓库锁定的 vcpkg manifest 构建 Release 并先跑全量 CTest。必要输入为指定提交的最小源码 zip、两个公开 dev TSV、配置及运行器；不传个人文件、本机专有二进制或 holdout。

`append_india_dev.py` 在远端下载并校验公开源归档，只重建清单选中的 dev 文件。按 `append-india-dev-r0.json` 串行比较追加、重建、固实及冷 / 热启动；每个产物独立往返。原始结果实时写到 `TASK_RESULTS/raw.jsonl`，产物大小和哈希写 `artifacts.jsonl`。`--analyze-only --results <目录>` 从原始数据重新生成 `summary.json`。这是探索性 dev 运行，不能直接作为留出结论。失败不覆盖或混入新运行，复现时保留任务实际输入快照与完整工具链版本。

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

## 追加 r2 资源容量

`append_resource_probe.py --tool <Release tool> --work <new dir> --results <new dir>` 使用固定随机数据测 128 / 256 / 512 MiB 内存、磁盘和往返，超预测门槛停止，不代表 Seed 收益。`append_resource_post.py` 在印度固定任务内构建、运行 Release / Sanitizer 与功能验收；容量负载应单独安排，避免构建和历史产物挤占技能磁盘预算。

`append_resource_compare.py <baseline probe results> <after probe results>` 校验完成 case 的八条命令、逐字节验证、输入与归档哈希，输出资源比较；部分 case 单独统计，不当成成功。完整示例见 [资源记录](../experiments/20261123-append-resource-main-001/record.md)。仅 Python 标准库。

## 大目录追加 dev r2

`append_large_manifest.py` 从已有 Linux 完整哈希索引沿用原 dev 连通分量归属，去重并生成嵌套 128 / 256 / 512 MiB 清单；每份经过生产 manifest 检查，输入不复制填充。见 [数据集](../datasets/append-linux-large-dev-r2.md)。

`append_large_dev.py` 是有独立 512 MiB / 100000 文件边界的新运行器，每次保留一个分支的旧新包，验证并记录哈希后回收；不修改 `append_history_r2.py` 的 8 MiB 功能上限。`check_append_large_dev.py` 用中文小夹具比较新旧调度的 34 个归档并检查确定性、父历史和回收。`append_large_task.py` 是本轮固定印度工具链的 bootstrap，绑定任务路径、源码和清单；不作为一般生产部署工具。

`append_large_report.py <dev-results> --output <new-json>` 从完整原始记录检查历史、预算搜索起点、代数及独立验证，汇总完整大小和工具进程成本。计时包括工具内部校验；单独的 verify 成本另列，准备输入 / 预热不包含在该表里。单遍成本测量不提供显著性或留出结论，详见 [本轮记录](../experiments/20261123-append-large-dev-main-001/record.md)。仅 Python 标准库。

本轮监控器发现并发删除文件竞态：`append_monitor_guard.py` 仅用于生成任务专用 runner 副本修复，并检查硬链接计数 / 消失文件 / 权限错误，不修改已安装 skill 或资源阈值。`append_large_continue.py` 绑定本轮原任务的固定数据 / 二进制哈希，只续测四个未完成分支。`append_large_merge.py <interrupted-dev> <continuation-dev> --output <new-dir>` 保留原命令的 task/index，生成明确标记的 aggregation.json；不向任何原任务写 result.json 或伪造完成状态。详细中断诊断与版本哈希见本轮记录。

## 成熟固实策略的 Seed 零搜索接入

`solid_seed_task.py` 为本轮印度固定工具链入口，执行 Release / ASan+UBSan、旧二进制兼容及真实 dev 验证；`solid_seed_zero.py` 对相同 Zstd-9 单组计划运行 legacy-solid / seed-solid / explicit-solid 三条独立历史，比较完整帧载荷和往返，不进行策略搜索。`solid_seed_report.py <zero-results> --manifest <fixed-tsv> --output <new-json>` 从40条原始命令核验15个产物与10次载荷比较，单独对账表示开销；只作零搜索封装等价结论。接口与当前限制见 [solid-plan-r1](../ideas/solid-seed-r1.md)，真实验证程度见 [执行记录](../experiments/20261123-solid-seed-main-001/record.md)。

## 固实预算搜索与复用

`solid_search_manifest.py <seedarc_path_check>`只移除既有512MiB dev清单的人工linux/前缀，保持内容与批次，并用真实group_key语义重新统计键数、运行生产manifest校验。

`solid_search_task.py`固定印度编译环境，Release全量 / 相关ASan后执行`solid_search_dev.py`：25条命令、10份独立verify产物、5次载荷对照，Seed / 显式各自初始搜索、固定默认、直接追加、热搜和冷搜，所有搜索预算4×输入字节。三份新增批次合并为一次更新；单遍dev，不是留出或显著性结论。

`solid_search_report.py <search-results> <manifest.tsv> <output.json>`独立重算每次搜索的候选最小值、确定性同分决胜、预算与轨迹，核对完整矩阵、物理字节对账和两表示同算子结果。原始日志 / 源码 / 环境和限制见[记录](../experiments/20261123-solid-search-main-001/record.md)。仅Python标准库。

## 固实混合数据与真实版本变化

`solid_version_data.py`在固定Linux dev路径投影中筛选正式6.12.5补丁，限制路径 / 模式 / 大小 / 重复项；仅对任务私有副本调用严格GNU patch，并按上游diff index核对新旧Git blob指纹、记录新SHA256和所有变化。`solid_version_task.py`复用固定印度工具链与DEV-019完全相同的C++源码zip，构建 / 回归与版本数据验收后再运行实验，不修改归档格式。

`solid_version_dev.py`在混合dev的v0分别全枚举29个Seed / 显式计划，再对实际v1比较固定默认、旧具体计划零搜索重压、8×热搜与8×冷搜。10份包全部独立verify，5对载荷比较，逐对回收归档并保留SHA / 原始指标。复用的是策略，输入仍完整重编码，不使用append覆盖同路径。

`solid_version_report.py <matrix> <v0.tsv> <v1.tsv> <version-delta.json> <output.json>`审计预算、初始全域最优值、4/8/29枚举前缀、真实继承起点、同算子对照和版本内容差异。枚举前缀是同一次运行内的累计观测，不能当作独立重复或与near算子直接归因比较。详见[数据集](../datasets/solid-mixed-version-dev-r1.md)及[DEV-020预登记 / 结果](../experiments/20261124-solid-version-main-001/record.md)。

混合清单可用`solid_version_manifest.py --path-tool <seedarc_path_check> --output research/datasets/solid-mixed-version-dev-r1`重建；拒绝覆盖不同清单，保留原始来源清单哈希。用户2026-11-24已明确速度 / 损坏恢复优先；上述体积搜索仅作机制基线，后续按[联合目标](../ideas/speed-resilience-r1.md)。

## 有界固实联合取舍

`bounded_solid_task.py`使用固定印度工具链，Release全量与相关ASan+UBSan、跨平台固定包哈希检查后运行`bounded_solid_dev.py`。整段 / 32 / 64 / 128MiB各自独立initial+3次新文件追加历史及最终重建，两表示压缩帧对照、独立verify、12个均匀路径加最大文件的提取诊断，以及10/50/90%载荷位置翻转1bit的条件损坏诊断。128命令 / 40包 / 20对载荷，验证后回收本任务包。

`bounded_solid_report.py <results> <manifest.tsv> <output.json>`从原始JSON审计帧上限 / 字节对账、旧包前缀与读写量、抽样集合和额外解码量、索引完好故障边界，生成每个配置的阶段表与生命周期编码成本。单遍dev，不作统计优势或用户预设冻结。工具能力边界见[说明](../ideas/bounded-solid-r0.md)，真实证据见[记录](../experiments/20261124-bounded-solid-main-001/record.md)。

## 有界固实平衡轮换复测

`bounded_repeat_task.py`复用同C++快照和固定印度工具链，Release全量、规范化固定包哈希检查后运行`bounded_repeat_dev.py`。四配置四轮循环平衡位置，偶数轮先完整重建再末批追加，奇数轮反向；每次追加从自己的同一第3代父包开始。所有输出SHA必须与DEV-021参考一致，逐次独立verify和固定样本access-profile。父包准备成本单列；未重复同包的显式编码 / 故障诊断，不产生新的安全性结论。

`bounded_repeat_report.py <repeat-results> <manifest.tsv> <reference-archives.json> <analysis.json>`独立检查120条命令顺序、44份包 / 父包引用、32组提取数据和同轮倍率，生成min/median/max。`bounded_repeat_preview.py <analysis.json> <preview.md>`渲染观测表。四次同数据 / 热缓存观察不等于统计显著性或跨数据保证，首轮数据不混入统计。方法和真实状态见[DEV-022记录](../experiments/20261124-bounded-repeat-main-001/record.md)。
