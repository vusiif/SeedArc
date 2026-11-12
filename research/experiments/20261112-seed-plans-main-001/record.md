# 20261112-seed-plans-main-001

状态：已完成计划输出 / 待用户语义评估。负责人：主 Agent；关联 DEV-005。时间、完整命令、候选 ID 与输入、可执行文件 SHA256 见 config.json。

问题：比较整体随机字段与分离等级字段的可观察影响，不检验压缩率。方法预先按候选 r1 规则选择 A 0→1、B codec=0 且 Zstd rank 2→3、LZMA2 rank=6，输出前 12 个块的计划。A/B 规则和固定向量见 source/research/ideas/seed-candidates-r1.md。没有文件数据集；块输入为 0..11 的固定全局序号。无预热、重复计时或搜索，不作性能结论。

代码基线 77764b9，本轮新源码与相关构建文件完整快照在 source/，哈希见 source-sha256.json。复现时以该提交独立 checkout 为底，覆盖本快照，配置 `-DSEEDARC_BUILD_RESEARCH_TOOLS=ON` 并构建 seedarc_seed_plan；本快照的完整 tests 目标还需本工作区 tests/fixtures/m3 固定样例。工具为本轮 MSVC Release，实际退出码与逐块JSON保留；输入不参与归档解码。

原始输出 a-r1.json / b-r1.json；人工阅读表 preview.md 由主线程从两份 JSON 按块合并生成，字段顺序相同。观察：A 变化10/12；B变化5/12，仅Zstd等级变化。不能推断最优位宽、字段布局、局部搜索效率或最终归档收益。

尚未由另一执行者独立复现；主线程构建和运行，子任务只实现代码并做静态格式核对。跨工具链与动态检查证据见 M4-A验证记录。下一步由用户评估字段语义与作用粒度，再决定后续候选。

## 可重复分析

`python research/scripts/seed_plan_table.py research/experiments/20261112-seed-plans-main-001/a-r1.json research/experiments/20261112-seed-plans-main-001/b-r1.json` 已执行，stdout 保存为 analysis-table.md；脚本副本 seed_plan_table.py 随本运行归档，SHA256 为 `11582ef27fadfa94243852d1ee9d6f3bf49586934dcd2ddc306fe72d0f2e3f30`。脚本只读原始 JSON 并校验块序号一致，不覆盖证据。
