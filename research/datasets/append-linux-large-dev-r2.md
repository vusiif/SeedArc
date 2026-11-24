# append-linux-large-dev-r2

2026-11-23，DEV-017 r2 的同来源大目录 dev 语料；来源为已有授权的 Linux 6.12。仅建立清单，实际内容待远端从固定归档提取并逐文件核验；不是正式留出评估。

保持 append-r1 的路径排除、共享内容连通分量与 `append-r1:` 哈希盐及 dev 归属，取消原 48 MiB 采样上限；从完整源码哈希索引选择 dev 内容，每个内容 SHA256 仅保留一个路径。按原路径哈希排序取不截断文件的嵌套前缀，再按原约 70/10/10/10 字节规则分批；边界处大文件可使批次比例偏离，实际数值在 [metadata](append-linux-large-dev-r2/metadata.json)。未打开留出内容或使用留出压缩结果选参；全量索引只用于保持既有分组 / 去重归属。

| 目标规模 | 实际字节 | 文件数 | 来源组数 |
| --- | ---: | ---: | ---: |
| 128 MiB | 134,159,235 | 7,939 | 270 |
| 256 MiB | 265,708,015 | 14,031 | 299 |
| 512 MiB | 536,864,155 | 30,134 | 329 |

三份清单均通过生产 `seedarc_path_check --manifest`（含隐含父目录）；内容无重复，不复制文件填充，不把目录规模当作搜索复杂度。未覆盖单个大文件、跨类型漂移或 B-mix。License 为 Linux 源码的 GPL-2.0 / 各文件 SPDX，元数据保存于仓库，内容及归档只保存于 research/local 或远端任务临时目录。

复现：`python -B research/scripts/append_large_manifest.py --listing research/local/append-r1/listings/linux-6.12-extracted.tsv --prior-metadata research/datasets/append-r1.json --path-tool <seedarc_path_check> --output <new-directory>`。固定 Linux 归档地址、SHA256、完整索引与原 metadata 哈希、各清单哈希见 metadata.json。脚本复用原建库代码的 components / key_hash / batches，原数据和清单未修改。
