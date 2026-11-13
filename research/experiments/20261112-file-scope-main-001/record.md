# 20261112-file-scope-main-001：全局与文件级等级覆盖

状态：功能性观察已完成，待用户评估；未进行压缩或搜索。负责人：主 Agent；关联 DEV-006。

## 问题、假设与范围

研究控制粒度：独立的等级覆盖层能否只改变目标文件内目标 codec 的拟议等级，并在清除后恢复全局默认。覆盖表是编辑配置，不是已经选定的紧凑种子。A/B-r1 保持原样；全局块 ID 与文件归属只针对当前固定清单，不承诺文件插入 / 重命名不变性。

假设：base → global-edit 改变所有 Zstd 块；base → file-edit 只改变选中文件的 Zstd 块；base → cleared 无差异。选中与未选中文件都必须有实际 Zstd 块，且目标文件必须出现至少一次真实等级变化，防止空操作被误认为隔离成功。

## 预设方法

固定 3 个假设文件，每文件 4 块；沿用 B-r1 codec=0、zstd_rank=2、lzma2_rank=6。比较全局 Zstd rank 2→3、仅 file_id=1 覆盖 Zstd rank=3、移除覆盖四组计划。记录每行的文件 / 局部块 / 全局块身份、codec 与等级、变化集合。不读取用户文件、不运行压缩、不测耗时或内存，不据此判断搜索效率。

代码、环境、执行命令和可执行文件哈希见 config.json。基线为 f3deb4a，工作区有本轮改动；新增源码、三处 CMake 文件、规则和分析脚本的完整快照在 source/，SHA256 见 source-sha256.json。以该提交独立 checkout 为底覆盖 source/，开启研究工具即可复现。原始 stdout 保留在 observation.json，stderr.log 为空；preview.md 由归档的分析脚本从原始 JSON 生成。没有文件数据集；块输入清单会保留在 JSON。可用性评估由用户随后完成；本轮自动检查只验证语义实现与边界。

## 执行与结论

主线程执行 MSVC Release 研究工具与分析脚本，均退出 0。全局 Zstd rank 2→3 改变 global IDs [1,3,5,6,8]；仅 file_id=1 覆盖改变 [5,6]；清除后相对基础计划无变化，相对文件覆盖改变 [5,6]。分析脚本核对实际逐行差异与声明集合，并要求选中和未选中文件都存在真实目标块，防止空操作通过。

复现命令（项目根目录）：

```powershell
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_scope_plan.exe overlay-r1 1 3 -
python research/scripts/file_scope_table.py research/experiments/20261112-file-scope-main-001/observation.json
```

所有参数为十进制；`-` 表示继承该字段。源候选仍为 B-r1，overlay-r1 是编辑层修订。没有选择最终 Seed 表示，不测实际文件压缩、元数据编码成本、搜索效率或峰值内存；config 中未测量字段为 null。OS / Python / CPU 说明与时间（America/Los_Angeles）保留在 config；不使用本样本宣称真实性能。

Actions / GUI / 块段控制 / codec 强制选择均不在本轮范围。用户原评估文档未改写，前一轮实验记录与 A/B-r1 输出保留不变。下一步由用户评估文件级粒度，随后决定是否继续覆盖语义探索或接入实际编码实验。

## 验证与独立性

子任务负责实现，未运行测试或 IDE；主线程独立构建、执行与核对结果。Release 首轮全量 33/33，修正头文件包含方式与测试 constexpr 后定向 3/3；ASan 定向 3/3；Clang Debug 定向 2/2；四组跨工具链输出字节一致。完整证据与限制见 [验证记录](../../../docs/file-scope-validation.md)。研究观察未由另一执行者独立复现；自动验证不代替用户对控制粒度的评估。
