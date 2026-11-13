# B2-r5：受预算约束的 Adaptive-file 选择器

日期：2026-11-12。实验规则（运行前登记），不冻结协议。输入：[B2-r4 评估与 B2-r5 实施计划](../../docs/review_results/SeedArc_B2-r4评估与B2-r5实施计划.md)，本轮只做 P0-A / P0-B / P0-C。前一轮结论见 [r4](seed-codec-allocation-r4.md)。

## 问题与可证伪假设

- 问题：在显式试编码预算下，按文件抽样选 Codec，能否以远低于 Oracle 的决策成本，接近 Oracle 的归档大小、且不比固定 Zstd-3 差？
- H1（不劣化）：任一预算档的 Adaptive-file 完整归档都不大于 Fixed Zstd-3。若某组更大，则 H1 被否定（规则会在抽样误判时选错）。
- H2（接近 Oracle）：无界预算档与 Oracle-file 的差距不超过 Oracle 相对 Zstd-3 节省量的一半。否定条件同字面。
- 不预设 H1 / H2 成立；本轮 5 组样本是开发样本，不作独立评估，**不据结果改参数**。

## 规则 `adaptive-file-r1`（运行前固定）

1. 每个普通文件默认 Zstd-3；块级无收益由 core 回退 Store。
2. 空文件 → `empty_file`；小于 16 KiB → `below_min_size`；均保持 Zstd-3，不试编码。
3. 抽样：块序号 `{0, ⌊n/2⌋, n−1}` 去重升序（最多 3 块，64 KiB 固定分块）。读取经 core 的 `InputTree` 安全路径（不跟随重解析点），读取字节记入证据。
4. 先试 Zstd-3 与 LZMA2-6。若该文件所需 2×抽样字节超出剩余预算 → 不试，`budget_exhausted`，保持 Zstd-3。
5. 试编码长度按 core 规则计：输出须严格小于原块，否则按原块长度计（等价于 Store 回退）。两者都无收益 → `no_gain`。
6. Zstd-3 抽样至少省 2 % 时，若剩余预算够再抽一次，才试 Zstd-6（“有明确收益时才考虑”）；不够 → 记 `zstd6_skipped_budget`。
7. 候选 c 取代 Zstd-3，须同时满足：抽样节省 ≥ Zstd-3 抽样字节的 2 %，且按 `文件字节 / 抽样字节` 外推的节省 ≥ 64 B。多个候选满足时取估计最小，平手顺序 Zstd-3 → LZMA2-6 → Zstd-6。否则 `insufficient_gain`。
8. 试编码出错 → `trial_failed`，保持 Zstd-3，不中止归档。
9. 文件按规范清单顺序处理，预算跨文件累计；决定只依赖文件内容、清单顺序与参数。

预算档（试编码输入字节，跨整个归档）：0、256 KiB、1 MiB、无界。0 档等价于“全部 `budget_exhausted` 的 Zstd-3”，用于核对选择开销。

元数据：选择结果完全由既有索引的逐块 Codec / config 表达，解码不重放选择，归档不新增字段，`metadata_bytes = 0`（与 B 方案“未序列化”的缺项不同）。决策证据只写研究文件。

## Oracle-file（参考上限）

对每个文件的**全部块**试 Zstd-3、Zstd-6、LZMA2-6，按 r4 规则选（LZMA2-6 更小则选之，否则 Zstd-3；若 Zstd-3 且 Zstd-6 更小则覆盖为 6）。试编码时间计入决策成本，不剔除；须与 r4 的 B2-file-codec 归档哈希一致。

## 结果

run [20261112-b2-adaptive-main-001](../experiments/20261112-b2-adaptive-main-001/record.md)：H1 在 5 组 × 4 档全部成立；H2 在 5 组都成立，但只有 source 是有意义的检验（Adaptive 8,300 B / Oracle 8,298 B / Zstd-3 9,503 B），其余四组 Oracle 与 Zstd-3 一样大。全部收益来自一个文件；在 binary / compressed 上的试编码成本没有换来任何收益。只是开发样本，须在独立评估集上检验（P1）。
