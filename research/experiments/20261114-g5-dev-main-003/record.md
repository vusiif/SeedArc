# 20261114-g5-dev-main-003：运行脚本检查（LZMA2-6 参照，r2 dev）

- **性质**：检查 `research/scripts/g5_run.py` 新加的 LZMA2-6 参照路径能否跑通，**不是比较实验**。只用 r2 dev，留出集没有动。
- **时间**：2026-11-14，紧接在 [002](../20261114-g5-dev-main-002/record.md) 之后。
- **网格**：三个池 × coord × 2× × 种子 1，加生产 CLI（`seedarc compress --codec lzma2 --level 6` → `verify` → `extract` → 逐文件比对）。命令见 `config.json`。
- **结果**：三个池的 LZMA2-6 归档都能逐字节往返（`references.json`）。生产归档字节数：docs 470,126、source-python 1,412,185、config 1,754,853。搜索结果与 002 的 2× 格一致，都停在起点。
- **注意**：生产归档的索引格式与研究容器不同（生产索引每个条目更大），两者直接相减会把格式开销算进“字典收益”。DEV-013 在报告时另外按生产索引大小做了折算，G5 正式报告也要同时给出直接值和折算值。
