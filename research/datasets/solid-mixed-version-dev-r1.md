# 固实混合数据与真实版本dev r1

2026-11-24，DEV-020。使用已登记的[公开dev数据](append-r1.md)与[Linux扩展dev](append-linux-large-dev-r2.md)，没有本机专有二进制或新增留出样本。

| 来源 | 文件数 | 字节 |
| --- | ---: | ---: |
| Linux6.12 原128MiB清单 | 7,939 | 134,159,235 |
| enwik8 既有dev片段 | 50 | 50,000,000 |
| Silesia dev的nci / reymont / sao / xml | 4 | 52,777,871 |
| 合计 | 7,993 | 236,937,106 |

v0清单：[v0.tsv](solid-mixed-version-dev-r1/v0.tsv)，SHA256 `5e006ba8ba6b626d0690841fbd2f06c31be0347c164a74773f6ce2016f2a484b`。沿用原文件路径 / 内容 / 归属，来源命名空间保持自然排序；enwik8的1MB切段仍是原实验预处理，不冒充自然文件。只排除Silesia的samba，以使完整源码 / 数据上传低于16MiB，在观察本轮结果前确定。v0没有内容SHA重复，完整生产路径清单通过（含父目录10,536条）。键计数及组成见[metadata.json](solid-mixed-version-dev-r1/metadata.json)。

v1是同一固定路径集合中的Linux6.12→6.12.5普通文件内容修改，其余两池保持不变；不包含未选新路径，不代表完整Linux版本树。正式累计补丁来自[kernel.org](https://cdn.kernel.org/pub/linux/kernel/v6.x/patch-6.12.5.xz)，[发布方校验表](https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc)给出的SHA256为`ff1db6fd2cf36772bc9136f96062ec6b3fff7d6858a512b6a446731dc242f10d`。下载由Gopeed完成并校验；没有声称独立验证PGP签名。筛选出127个原选文件修改，正式运行时必须严格应用并核对Git blob指纹，才可以把v1用于测量。

`solid_version_data.py`限制补丁解压大小、路径、普通文件模式和重复目标，只在独立复制的任务工作区应用`patch --fuzz=0`，拒绝偏移 / 错误；同时检查上游diff index的新旧Git blob前缀及所有文件的新SHA256、未选文件不变。完整源和补丁正文留在忽略的本地结果包 / 远端保留结果中，不作为项目第三方实现提交。许可沿用原公开数据集说明；不新增公开再分发承诺。

v1已严格验收：127文件修改、7,866文件不变，输入236,963,618B，比v0增加26,512B；[v1.tsv](solid-mixed-version-dev-r1/v1.tsv)保存逐文件SHA256，清单哈希见metadata.json。实验范围、执行证据和实际完成状态见[DEV-020记录](../experiments/20261124-solid-version-main-001/record.md)。策略复用仍完整重压快照，不代表归档已支持同路径覆盖、删除或重命名。
