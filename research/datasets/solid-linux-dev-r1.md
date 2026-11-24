# 固实搜索 Linux dev r1

沿用 [append-linux-large-dev-r2](append-linux-large-dev-r2.md) 的512MiB清单：同30,134文件、536,864,155字节、内容SHA256、原始dev归属和四个批次。仅移除相对路径及来源组名的人工`linux/`前缀，不读取留出内容、不新增或复制样本。来源仍为Linux6.12，GPL-2.0及各文件SPDX适用。

[dev.tsv](solid-linux-dev-r1/dev.tsv) SHA256为`eec9c12fc477dd8852577e0ad1c41c015a7c1c3bd4fd9f5c56eca10d7a517c8e`。生产manifest检查通过，含父目录33,905条。复现：`python -B research/scripts/solid_search_manifest.py <seedarc_path_check>`；脚本拒绝覆盖不同清单。[metadata.json](solid-linux-dev-r1/metadata.json)保留原清单哈希和逐批次字节数。

按C++真实group_key语义统计，一级 / 二级目录键分别从1 / 22变为22 / 326，扩展名100、大小类别3不变。旧辅助统计把root文件错误算作额外二级键，写成25；此处显式更正为22。旧压缩实际使用C++规则，其原始字节数和耗时不受影响，不覆盖旧原始统计。

路径改变会影响归档路径字段、索引和分组，因此不能将本轮与旧路径包的大小差直接归因为搜索收益。本轮内部所有方法使用同一新清单。首次测量将原三份追加合并成一次更新；这不代表真实文件修改或三步漂移。
