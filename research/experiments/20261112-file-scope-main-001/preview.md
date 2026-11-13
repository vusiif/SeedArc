# 文件级等级覆盖对照

只比较拟议计划，不代表压缩或搜索收益。

全局编辑改变块：`[1, 3, 5, 6, 8]`；文件覆盖改变块：`[5, 6]`。
清除后与基础计划完全一致；对选中与未选中文件均存在真实目标块的情况已核对。

| 文件 | 文件内块 | 全局块 | 基础 | 全局修改 | 文件覆盖 | 清除覆盖 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | lzma2-6 | lzma2-6 | lzma2-6 | lzma2-6 |
| 0 | 1 | 1 | zstd-3 | zstd-4 | zstd-3 | zstd-3 |
| 0 | 2 | 2 | store | store | store | store |
| 0 | 3 | 3 | zstd-3 | zstd-4 | zstd-3 | zstd-3 |
| 1 | 0 | 4 | lzma2-6 | lzma2-6 | lzma2-6 | lzma2-6 |
| 1 | 1 | 5 | zstd-3 | zstd-4 | zstd-4 | zstd-3 |
| 1 | 2 | 6 | zstd-3 | zstd-4 | zstd-4 | zstd-3 |
| 1 | 3 | 7 | store | store | store | store |
| 2 | 0 | 8 | zstd-3 | zstd-4 | zstd-3 | zstd-3 |
| 2 | 1 | 9 | lzma2-6 | lzma2-6 | lzma2-6 | lzma2-6 |
| 2 | 2 | 10 | store | store | store | store |
| 2 | 3 | 11 | lzma2-6 | lzma2-6 | lzma2-6 | lzma2-6 |

本轮没有修改 codec 调度，没有验证跨文件插入 / 重命名的身份稳定性。
文件覆盖是独立编辑层，尚未选定如何编码进种子或用于搜索。
