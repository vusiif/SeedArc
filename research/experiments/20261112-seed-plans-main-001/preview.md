# 第一轮种子计划对照

状态：功能性观察，未比较压缩率或搜索效率；用户语义评估尚未完成。

所有输入字段为候选 r1 实验编辑格式，不是归档种子。BLOCKS 是假设的固定块清单，不读取真实文件。

- A：value 从 0 改为 1，12 块中 10 块拟议计划变化。
- B：codec 字段保持 0，Zstd rank 从 2 改为 3（等级 3 → 4），LZMA2 rank 保持 6。12 块中 5 块拟议计划变化，均为原 Zstd 块；codec 分配不变。

| 块编号 | A 修改前 | A 修改后 | B 修改前 | B 修改后 |
| --- | --- | --- | --- | --- |
| 0 | lzma2-2 | store | lzma2-6 | lzma2-6 |
| 1 | zstd-2 | zstd-2 | zstd-3 | zstd-4 |
| 2 | store | zstd-14 | store | store |
| 3 | zstd-9 | store | zstd-3 | zstd-4 |
| 4 | lzma2-7 | store | lzma2-6 | lzma2-6 |
| 5 | zstd-13 | store | zstd-3 | zstd-4 |
| 6 | zstd-1 | zstd-19 | zstd-3 | zstd-4 |
| 7 | store | lzma2-4 | store | store |
| 8 | zstd-4 | zstd-14 | zstd-3 | zstd-4 |
| 9 | lzma2-4 | lzma2-3 | lzma2-6 | lzma2-6 |
| 10 | store | store | store | store |
| 11 | lzma2-1 | zstd-19 | lzma2-6 | lzma2-6 |

B-r1 的等级对同一 codec 的所有块统一生效，不能单独调一个块；这正是后续请用户评估的限制。A 支持逐块随机等级，两者能力与采样分布不同，不能拿本表推断公平搜索优劣。两者都未执行压缩，因此没有实际 Store 回退、归档字节数、搜索耗时或峰值内存结论。

待用户比较：希望修改等级影响同 codec 的所有块、单个文件，还是指定块段？现有字段是否容易理解？codec 字段扰动影响范围是否合适？先体验这些行为，再决定是否引入候选 C 或调整 B 的粒度；本轮不决定最终编码。
