# B2-r3：具体计划的真实压缩接入

日期：2026-11-12。实验规则，不冻结协议。输入：[r3评估计划](../../docs/review_results/SeedArc_B2-r2评估与B2-r3真实压缩验收计划.md)。

## 实现边界

生产核心新增内部 archive_plan.hpp：具体 `BlockEncoding{Codec,config}`，以及在真实安全枚举后、读取文件内容前调用一次的 PlanBuilder。视图只在调用期间有效，返回计划必须等于真实块数且每条codec/config合法；失败、取消和输入变化沿用事务清理，不能发布部分归档。该入口只供受信进程内调用，不是插件或对外策略协议。

普通 compress_archive 继续固定参数，不解释Seed。研究库 archive_study 将同一份真实清单的普通文件（含空文件）与块身份交给原 catalog-r1/overlay-r1，返回具体计划；全局块ID在本次归档中等于0..N-1序号，file_id是清单条目编号。路径仍是覆盖键。r2夹具中的105是逻辑global_block_id，不是9行数组的下标；输出必须区分block_index / block_id / file_block_index / path。

未知B候选ID/修订或catalog修订拒绝；继续保留A/B-r1、路径与覆盖原行为。核心接收当前计划，不知道种子的位宽/表示。相同文件清单与块大小下，b-base/global/file/cleared共用codec_seed0，只修改等级；文件覆盖优先，清除恢复当前全局。

仍使用已有索引保存实际Codec/config。Header strategy_version=0、seed_length=0，解码不依赖Seed、外部配置或搜索；覆盖参数与计划证据外置研究归档。因此本轮不是Seed持久格式或最终元数据成本评估。实际Store回退与计划分开记录。不同Codec/config切换时释放旧编码器，只保留一个活编码器；不新增缓存或编码器池。

## 预设实验

64KiB固定块、单线程。Store/Zstd3/Zstd6/LZMA2-6作为单算法参考；同Codec分配的B-base(Zstd3)/global(Zstd4)/file(目标Zstd6)/cleared(global4)作为严格文件覆盖对照。每组3次轮转顺序，无搜索、无预热，OS缓存不受控。

输入在磁盘上生成，包括项目源码原件、自制文本/二进制/gzip以及含空文件与Unicode路径的混合目录。固定目标不根据压缩结果挑选。所有成功样本运行compress→verify→extract并分块逐字节比较，保留文件哈希和完整归档哈希。工程验收不要求收益为正。

计时包括进程启动、计划生成、编码与自校验及JSON证据；verify和extract各自独立进程。峰值内存为GetProcessMemoryInfo的PeakWorkingSetSize，包含工具与报告，不能解释为纯codec内存或系统总峰值。Release测量；Debug只验证语义。

事实与快照见 [实验记录](../experiments/20261112-b2-compression-main-001/record.md)。测试纳入CTest，Actions与GUI按用户要求延后。功能与数据验收分开；下一步仍需代表性真实语料与公平预算，不能从本轮推出搜索收益。
