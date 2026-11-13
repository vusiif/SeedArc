# B2-r4：Codec 分配诊断与简单基线

日期：2026-11-12。实验规则与结论摘要，不冻结协议。输入：[B2-r3 首轮评估与 B2-r4 优化计划](../../docs/review_results/SeedArc_B2-r3首轮评估与B2-r4优化计划.md)，本轮只做 P0-A / P0-B。

## 问题与可证伪假设

- 问题：B2-r3 中 B 策略在 text / mixed 上远差于固定 Zstd-3，损失来自哪里？
- 假设 H1：主要来自 B-r1 与内容无关的 `hash mod 3` 分配把可压缩块计划成 Store。若计划 Store 块的字节差不足总差距的一半，则 H1 被否定。
- 假设 H2：每文件选一个 Codec（再允许文件级等级覆盖）至少回收 H1 的损失。若 B2-file-codec 在 text / mixed 上仍明显大于 B1，则 H2 被否定。

## 结果（run [20261112-b2-diagnose-main-001](../experiments/20261112-b2-diagnose-main-001/record.md)）

- H1 成立：计划 Store 块占 text 差距 65,450 / 65,594、mixed 66,759 / 66,903（均 99.78 %）；其余为 LZMA2-6 每块比 Zstd-3 多 72 B。等级 3 vs 4、头部和索引无贡献，块边界相同。
- H2 成立：B2-file-codec 在 text / mixed 与 B1 相同（944 / 394,959 B）；source 上 8,298 B，比所有固定方案和 b-file 都小，但只比 lzma6 少 6 B，是单样本、3 配置 oracle 的结果。文件级等级覆盖本轮只多省 2 B。

## 对后续的含义（建议，未实施）

- B-r1 的 Codec 字段让 Store 成为 1/3 概率的“计划”，而 Store 本来就是 core 的自动回退；把 Store 从可计划 Codec 中去掉（或仅作显式选项）是最直接的修正候选，需新候选修订，不能重解释旧种子。
- 在没有内容信号时，随机 Codec 分配在这组样本上没有正面作用；后续若比较 Seed 搜索，基线至少应包括 B1 与 B2-file-codec，并计入后者约 3 倍的试编码成本和 B 方案的 Seed / 覆盖元数据。
- 先扩充代表性语料（P1），再决定是否改策略空间；本轮不改算法。
