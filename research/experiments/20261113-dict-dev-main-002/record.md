# DEV-013：共享字典 dev 调参（选参用）

日期：2026-11-13。规则登记在先：[shared-dictionary-r1](../../ideas/shared-dictionary-r1.md)。语料是 [b2r5-corpus-r2 修订 2](../../datasets/b2r5-corpus-r2.md) 的 **dev** 划分；test 划分没有读取。上一次尝试 run `20261113-dict-dev-main-001` 中途失败，原因是生产归档器拒绝了一个含 `~` 的路径。失败记录保留（见其 config.json 的 `post_mortem`），本次是修正语料之后的重跑。

- **网格**：2 个生产参照组（zstd3 / lzma6）；级别 3 和 9 各一个无字典容器；字典组为 D ∈ {16, 64, 112, 256} KiB × 训练预算 T ∈ {1 MiB, 4 MiB, 全部输入}。共 28 组配置 × 3 个数据池 × 3 次重复，252 次运行，全部逐字节一致，同组的产物完全相同。
- **选参**（按登记的规则：每个级别取三个 dev 数据池的完整容器字节之和最小者）：
  - 级别 3：**D=64 KiB，训练用全部输入**，dev 合计 3,442,393 B；
  - 级别 9：**D=64 KiB，训练用全部输入**，dev 合计 3,141,686 B。
  - 选参结果写在 `selection.json`。test 评估在此之后才运行，并且只运行了一次：[20261113-dict-test-main-001](../20261113-dict-test-main-001/record.md)。
- **dev 上的观察**（只作参考，不作结论）：
  - 256 KiB 字典在三类数据上都更差，因为字典字节吃掉了收益；
  - 16 KiB 在 many-small-docs 上最好，但合计不如 64 KiB；
  - T=1 / 4 MiB / 全部之间差异很小。
- 完整表格见 [preview.md](preview.md)，原始数据见 raw.json / summary.json。
