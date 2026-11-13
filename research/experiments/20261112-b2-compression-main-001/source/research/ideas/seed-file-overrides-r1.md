# 文件级等级覆盖：overlay-r1

日期：2026-11-12。状态：**控制粒度研究原型规则，未选定最终 Seed 设计**。本文件定义行为；实际构建、测试、IDE 与观察结果由协调者另行归档，不把实现或单测的存在当作验证通过。

输入：[第一轮评估与建议](../../docs/review_results/SeedArc_种子计划对照评估与下一轮实验建议.md)、[A/B-r1 规则](seed-candidates-r1.md)。本轮保留 A/B-r1 源码、规则和固定向量，不增加块段或单块编辑，不研究 Codec 重采样，不改归档或生产 CLI。

## 1. 定位

overlay-r1 是 B-r1 具体计划之上的**独立编辑覆盖层**。它允许全局默认加指定文件的可选等级字段，不是新候选 B2 的紧凑 Seed 编码，也不表示已解决如何用少量种子表达覆盖列表。归档元数据成本、压缩效果、搜索优劣和实际 Store 回退均未由此实验测量。

生成仍调用原 `make_plan({field_candidate,1}, fields, global_block_id)`。覆盖只替换对应文件的 `zstd_rank` / `lzma2_rank`，不改 codec 字段，不改变算法分配，不重采样。A-r1 可独立保留，本轮覆盖接口只接受 B 的语义默认字段。

## 2. 输入、身份与限额

`make_file_plans` 接受 B-r1 默认字段、块清单、文件覆盖清单和 overlay 修订号（目前仅 1）。块身份包含：

- `file_id`：当前固定清单中的文件归属；
- `file_block_index`：该文件内的块标识；
- `global_block_id`：原 B-r1 用于映射的全局块标识。

三者均为无符号 64 位，允许稀疏值和最大值；`global_block_id` 全局唯一，`(file_id,file_block_index)` 组合唯一。输出按原 global ID 排序，**排序不重新编号**，也不要求各 ID 连续。

已知文件集合从块清单推导。本原型没有完整文件树，不提供空文件的编辑行为；无块文件不在可覆盖集合内。文件 ID 的稳定性只限这份固定清单，不承诺插入文件、改分块或重命名后的身份稳定。

上限：65536 块、4096 个不同文件、4096 条覆盖。数量超限在生成前明确返回 `resource_limit`。验证通过排序、集合和映射完成，时间 `O(B log B + O log O)`，存储 `O(B+O)`，没有逐块扫描全部覆盖项。B/O 分别表示块数和覆盖条目数，文件数不超过块数。

未知 overlay 修订返回 `unsupported_feature`。非法默认等级、重复块身份、重复覆盖文件、未知文件 ID、非法等级或没有任何字段的空覆盖均为 `invalid_argument`。有效空块清单加空覆盖允许产生空计划；非法默认值在这种情况下也必须拒绝。

## 3. 逐字段继承与清除

每条覆盖为 `(file_id, optional zstd_rank, optional lzma2_rank)`。等级范围仍为 Zstd rank 0..18（实际 level 1..19）与 LZMA2 rank 0..9（实际 preset 0..9）。

- 未出现的文件完全继承当前全局默认。
- 出现的文件按字段继承：仅覆盖 LZMA2 rank 时，该文件的 Zstd 块仍随全局 Zstd rank 变化。
- 被覆盖字段不随全局同字段变化；未选择的文件仍跟随全局变化。
- 删除一个可选字段使该字段恢复当前默认；另一覆盖字段继续有效。
- 删除整条覆盖使该文件全部恢复当前默认。默认改变后清除，恢复的是新默认，不是创建覆盖时保存的旧值。
- 两字段都空的条目不表示清除；必须移除该条目，避免无效状态并存。

覆盖列表顺序和输入块行顺序都不影响规范输出。在匹配文件中，Zstd rank 只改变 Zstd 块，LZMA2 rank 只改变 LZMA2 块，Store 不变。拟议等级不保证实际压缩会使用该 codec；Store 回退须等真实编码时独立记录。

## 4. 对照工具

`SEEDARC_BUILD_RESEARCH_TOOLS=ON` 时增加 `seedarc_seed_scope_plan`；沿用已有研究库，不引入新依赖。

```powershell
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_scope_plan.exe --help
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_scope_plan.exe overlay-r1 1 3 -
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_scope_plan.exe overlay-r1 1 - 3
```

命令参数为 `overlay-r1 FILE_ID ZSTD_RANK|- LZMA2_RANK|-`。**本工具的 ID / rank 使用十进制**，与上一轮工具的十六进制字段编辑格式有区别，`--help` 明确说明。`-` 为该字段继承，至少一个字段必须有值；错误参数退出 2，输出 JSON 前完成输入验证。

固定夹具为 3 个假设文件（ID 0/1/2），每个 4 块；文件 ID 为 global ID 整除 4，文件内 ID 为 global ID 模 4。默认 B-r1 fields 为 `(codec=0,zstd_rank=2,lzma2_rank=6)`。选择文件 1 时，其 Zstd 块为 global 5/6，未选文件也有 Zstd 块，避免空操作造成虚假“隔离通过”。这不是实际文件输入。

一个命令输出四份计划：

| 状态 | 行为 |
| --- | --- |
| base | 原全局默认，无文件覆盖 |
| global_edit | 将本次给出的非空 rank 应用于全局默认，无文件覆盖 |
| file_edit | 全局默认不变，仅选择文件应用这些 rank |
| cleared | 移除文件覆盖，恢复原全局默认 |

JSON schema 为 `seedarc.seed-file-scope.r1`，`plan_only=true`。包含 `overlay_revision`、`base_candidate`、夹具 ID、完整默认和覆盖配置；每个块行带三个身份与四份 `{codec,level}` 计划。`changed_global_ids` 中 `global_edit` / `file_edit` / `cleared` 都相对 base，`clear_from_file_edit` 相对 file_edit。清除后的集合相对 base 应为空，但清除相对 file_edit 应列出恢复的块。

例 `overlay-r1 1 3 -` 的预期：全局修改影响 Zstd 块 1/3/5/6/8；文件覆盖只影响 5/6；清除恢复 base。本例预计变化集合仅用于检查程序与用户是否理解作用域，不代表压缩收益。

## 5. 本轮检查与后续评估

单测针对两个 codec 隔离、文件隔离、逐字段继承、默认改变时继承 / 覆盖差别、逐字段与整条清除、顺序无关、稀疏 / 最大 ID、重复 / 未知 / 空覆盖 / 修订 / 等级 / 数量边界。旧 A/B-r1 测试继续保留。

协调者统一构建、动态检测、CLion 诊断和实验归档。人工评估重点：是否能预判全局编辑与文件编辑的不同变化集合；清除是否符合“恢复当前默认”的理解；文件级粒度是否足够。没有证据前不加入 B3、候选 C、自动搜索或持久元数据设计。
