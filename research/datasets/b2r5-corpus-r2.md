# b2r5-corpus-r2

修正 [r1](b2r5-corpus-r1.md) 按路径划分导致 dev / test 内容重叠的问题。清单与环境见 [JSON](b2r5-corpus-r2.json)，生成与检查入口为 `research/scripts/b2_corpus_r2.py`。

## 划分与范围

按项目组划分；共享内容 SHA256 的组（包括跨数据池）合并为一个连通分量，再按分量键的固定哈希分配到 dev 或 test。空文件排除。当前只有 CMake 文档、Python 标准库源码和 CMake / 包配置三个数据池，不等同于 r1 的十类语料。

原始文件仅保存在被忽略的 `research/local/b2r5-corpus-r2/`，不随仓库发布。仓库仅保存相对路径、大小、SHA256 和分组清单；来源与许可边界见 JSON。本机文件版本变化后不能假定可逐字节重建。

## 验证与限制

2026-11-12 提交整理时实际运行 `python research/scripts/b2_corpus_r2.py --check`：dev 2,434 种内容、test 2,290 种内容、717 个分量，内容交集为 0。六份 TSV 的 SHA256 均与 JSON 一致，Python 语法与 JSON 解析通过。

上述检查针对已保存的清单，不是重新核对全部本机原始文件，也不是压缩评估。尚未运行 r2 的字典训练、压缩往返或性能比较；不据此声明收益或完整独立验收通过。

## 修订 2（2026-11-13，DEV-013）

第一次 dev 调参（run `20261113-dict-dev-main-001`，状态 failed，记录保留）中途失败：生产归档器拒绝了路径 `msys2-etc/pacman.d/gnupg/pubring.gpg~`，因为生产路径规则禁止 `~`。研究容器接受了它，两边比较的输入就不一致。

修正：建库脚本新增 `archive_path_ok()`，与生产 Windows 路径规则（`core/filesystem/archive_path.cpp`）一致，过滤掉生产归档无法保存的路径，并在清单的 `path_rejected_by_archive` 中逐个列出。按修订版重建后：

- 只有 dev / config-cmake-pkg 少了这 1 个文件（1432 → 1431）。test 划分的清单与修订 1 **逐字节相同**。
- 不相交检查依然通过：dev 2,434 种内容、test 2,290 种内容、交集 0。
- 修订 1 的清单保留在 git 历史中（提交 `9284eae`）。修订发生在 test 划分任何测量之前。

## 使用记录

- dev 划分：DEV-013 共享字典调参（run `20261113-dict-dev-main-001` 失败、`-002` 完成）。
- test 划分：DEV-013 正式评估 **一次**（run `20261113-dict-test-main-001`）。以后不能再用它调参；新的方法需要新的留出集。
