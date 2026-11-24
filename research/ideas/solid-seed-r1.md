# solid-plan-r1：在成熟固实策略上附加 Seed

2026-11-23，DEV-018。依据用户最新要求，[主方案 §9](seed-append-main-r2.md#9-用户最新定位在成熟策略上使用-seed重点测大规模)。本轮为私有研究原型，未冻结生产格式或长期 Seed 长度。

## 最小范围

既有 Zstd-9 固实帧编码器继续工作；Seed 和显式描述只负责提供相同的分组计划。默认计划 0 是单组固实。追加 segment 保留旧帧并给新批次生成帧，regroup 重写所有组；二者不增加搜索。先证明零搜索时载荷相同，再在大规模探索优化。

旧 v4 的 seed / solid 模式和 G5 种子映射 revision1 不变。研究工具仍名为 seedarc_append4_study，新两模式使用头部数值修订 **5**；沿用 SADXR004 / GEN4 / END4 等记录标签族及有界索引结构，头部修订负责区别语义，旧解码器应拒绝修订5。JSON 输出模式沿用工具原有 schema 家族；pack / append 的 container_revision 与 strategy_revision 字段标识实际档案与策略版本。

## 表示与具体计划

固实 Seed 映射 **revision2**（候选名 solid-plan-r1）：四字节小端整数，仅当前低五位有效；低两位为既有 rule（dir1 / dir2 / ext / size），后三位为 k−1（1–8 组）。k=1 只允许 rule=0，其他高位必须为0。当前共有29个规范值，0 / 6 / 11 / 31 为测试向量，1 / 2 / 3 / 32 / 64 等被拒绝。该范围是本轮有界候选，不代表最终 Seed 宽度 / 长度或已经难搜索。

显式描述为四个字节：layout=1（solid）、rule、k、level=9。读取后与 Seed 展开为相同具体 Plan，字典 / 训练位段均为0；共享 encode_pending → compress_solid，无复制 codec 实现。表示与展开集中在 research/tools/solid_plan.hpp，codec / 文件 I/O 不解释 Seed 位段。

两种表示每代均保存可重建计划：GEN 的 rev / len 为 Seed 的 2 / 4 或显式的 0 / 4，随后写对应四字节值。键表仍保留，解码时由记录中的计划和键表核对每个文件应属的帧组；未知修订、非规范值、非零字典和帧组不一致均拒绝。新一代可以用自己的计划，旧代不重新解释。

无搜索默认调用示例：

```text
seedarc_append4_study pack solid 0x0 INPUT BASE.sadx off
seedarc_append4_study pack seed-solid 0x0 INPUT SEEDED.sadx off
seedarc_append4_study pack explicit-solid 0x0 INPUT EXPLICIT.sadx off
seedarc_append4_study payload-compare SEEDED.sadx BASE.sadx
seedarc_append4_study payload-compare SEEDED.sadx EXPLICIT.sadx
```

两种新表示都使用同一稳定组映射；旧 v4 solid 的最近组追加启发式保持不变。因此与旧 solid 的完整追加历史直接比较默认单组，其他分组先比较两种新表示的同策略历史，不把不同追加分组启发式的差异混为 Seed 代价。新计划异代后 regroup 要求所有代计划 / 键表一致，否则拒绝，重新按新计划整体 pack 是另一条明确路径。

## 开销、限制与验收

新 Seed 和显式描述当前同为四字节，无预设表示压缩收益。相对旧 solid，新增每代四字节计划值，前部与当前尾部各保存一次；旧尾部保留会累积这些副本。完整归档与帧载荷分别核算，不能把 Seed 加上去就当成更高压缩率。

首版仅 off 冗余；partial 恢复和 split 分卷明确拒绝，不继承未经验收的功能声明。旧 search 命令仍针对旧分块 / 字典目标，本轮没有固实搜索器，不把它用于新策略。输入 / 帧 / 文件数限额及 Unicode 路径校验不变，仍非完整流式处理，不升级 >1 GiB 能力。

验收包括固定向量、负面元数据和损坏帧、中文 / 空文件 / 多块内容、重复字节一致、segment / 新代 / regroup 往返、旧包不变、Seed / 显式及默认旧固实的压缩帧字节一致，以及旧 v4 三模式的双向兼容。Windows / Linux 和动态分析的实际完成程度、512 MiB 零搜索数据见 [执行记录](../experiments/20261123-solid-seed-main-001/record.md)，不把计划写成完成证据。

## DEV-019：在同一固实管线上搜索

后续研究入口为`solid-search seed|explicit START INPUT OUT BUDGET PRNG [near|enumerate]`，不更改上述序列化。Seed起点为规范十六进制值，显式起点为`rule:k`直接具体计划，二者可用`default`。显式pack新增同一`rule:k`入口，保留旧十六进制简写用于兼容测试。

每次未缓存试编码都调用现有完整固实pack，按输入字节数收费，全空文件使用1字节虚拟单位；成功候选都逐字节验证，时间中包含内部验证。起点先评估，单组强默认必评；不同起点至少预付两次预算。选择完整归档最小值，同大小按rule/k排序，最终直接保存已验证的最佳包。保留29个评分和当前最佳包，未缓存全部候选包。参数、预算或候选验证失败不会发布归档，写出沿用既有安全write_new路径。

near-r1是围绕起点的固定距离排序扫描：`abs(k-start.k)+(rule!=start.rule)`，相同距离由固定SplitMix64序列及rule/k排序决定；不是自适应爬山，也不声称局部最优。enumerate按rule/k遍历，用于小夹具全29计划参考。缓存仅本次搜索有效。当前空间很小，规模增加主要增加试编码成本，不自动变成复杂空间。

初轮dev使用[相对根路径清单](../datasets/solid-linux-dev-r1.md)，4次完整编码预算；比较固定默认、旧计划直接分段追加、旧计划热启动全量搜索与默认冷搜索。Seed / 显式各保留自身初始历史，同起点 / 算子 / 预算，不把两种表示本应相同的搜索结果归为Seed独立优势。实验事实与动态检查范围见[DEV-019记录](../experiments/20261123-solid-search-main-001/record.md)。
