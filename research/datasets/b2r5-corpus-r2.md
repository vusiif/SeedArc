# b2r5-corpus-r2

修正 [r1](b2r5-corpus-r1.md) 按路径划分导致 dev / test 内容重叠的问题。清单与环境见 [JSON](b2r5-corpus-r2.json)，生成与检查入口为 `research/scripts/b2_corpus_r2.py`。

## 划分与范围

按项目组划分；共享内容 SHA256 的组（包括跨数据池）合并为一个连通分量，再按分量键的固定哈希分配到 dev 或 test。空文件排除。当前只有 CMake 文档、Python 标准库源码和 CMake / 包配置三个数据池，不等同于 r1 的十类语料。

原始文件仅保存在被忽略的 `research/local/b2r5-corpus-r2/`，不随仓库发布。仓库仅保存相对路径、大小、SHA256 和分组清单；来源与许可边界见 JSON。本机文件版本变化后不能假定可逐字节重建。

## 验证与限制

2026-11-12 提交整理时实际运行 `python research/scripts/b2_corpus_r2.py --check`：dev 2,434 种内容、test 2,290 种内容、717 个分量，内容交集为 0。六份 TSV 的 SHA256 均与 JSON 一致，Python 语法与 JSON 解析通过。

上述检查针对已保存的清单，不是重新核对全部本机原始文件，也不是压缩评估。尚未运行 r2 的字典训练、压缩往返或性能比较；不据此声明收益或完整独立验收通过。
