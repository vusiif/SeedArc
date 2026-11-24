# SeedArc 交接快照

更新：2026-11-24，America/Los_Angeles。供下一位 Agent 接续开发使用。

**当前进度唯一入口仍是 [STATUS.md](STATUS.md)。本文件是交接快照，不另建任务台账；后续任务领取、结果和阻塞继续更新 STATUS。**

## 1. 接手先读

1. [AGENTS.md](AGENTS.md)、[STATUS.md](STATUS.md)、`git status --short`。
2. 用户最新方向：[速度 / 体积 / 恢复性联合目标](research/ideas/speed-resilience-r1.md)。
3. 当前实现：[有界固实帧说明](research/ideas/bounded-solid-r0.md)。
4. 最新证据：[DEV-022记录](research/experiments/20261124-bounded-repeat-main-001/record.md)、[复测表](research/experiments/20261124-bounded-repeat-main-001/preview.md)。
5. 按任务读取相关代码 / 详细规范；研究开始前读 [research/README.md](research/README.md)。

`docs/users_idea/` 原稿两份哈希最近复核未变，已读快照在 STATUS。**最新聊天决定及测试后形成的 Markdown 优先于早期构想**，不能只通读原稿后忽略新方向。关键设计或交接前继续核对新增 / 变化文件，不擅改原稿。

## 2. 用户已经明确的方向

- 目标是**少量甚至零牺牲，换取明显优势**：接近固实压缩率时提高速度、损坏隔离、恢复或单文件提取能力；或在相同总压缩时间内，得到比其他方案更小的完整归档。
- 用户应能选择优先速度、体积、提取、追加等取舍；不要替所有人固定一种权重或只用对手的单项极值作判据。
- 核心场景并列：首次压缩 / 全量解压、补加新文件、单文件提取、损坏 / 缺卷恢复。追加不是提取的替代项。
- 成熟 codec 和有效固实策略打底，Seed 描述、选择和复用具体计划。不要为了证明 Seed 有效而人为制造复杂空间，或反复在小空间抠几 KB。
- 规划 / 搜索、训练、索引、校验、冗余、整理、旧包读写都须计入对应成本；不能把定期全量重压挪到别处而称追加免费。
- 用户已明确：**当前阶段暂不考虑 CLion 建议性检查**，不再要求其切到前台；工程安全、真实构建和往返检查仍保留。

## 3. 当前状态与工作区

- 分支：`main`；接手核对时 HEAD 为 `479eab9a524ff0cf82ef55f36dc19ea4d06f77fc`。
- DEV-022 四轮轮换复测已完成；研究选项已列出，未设置通用默认。
- 交接起草时有多阶段研究、源码、清单和证据尚未提交；随后用户已授权整理提交并推送到 `main`。实际提交与远端同步状态以 Git 历史和实时检查为准；临时测试目录与调试日志不纳入提交。
- 交接文档阶段只新增本文件并在 STATUS 链接；后续提交整理不修改 C++ 或重跑性能实验。
- 前轮任务据收尾记录已退出、取回、核验并清理自己的 work/tmp；此后未再启动测试。远端其他任务的最新状态需接手时 `preflight` 核实，不能把记录当实时监控。

## 4. 已实现的最小能力

核心研究代码：

| 文件 | 职责 |
| --- | --- |
| [append4_study.cpp](research/tools/append4_study.cpp) | 私有归档研究工具、编码 / 追加、已有解析与验证；新增帧上限选项 |
| [bounded_solid.hpp](research/tools/bounded_solid.hpp) | 单文件读取 / 解码计量、索引完好载荷损坏诊断 |
| [solid_plan.hpp](research/tools/solid_plan.hpp) | solid-plan-r1 / Seed映射revision2与具体计划边界 |
| [check_bounded_solid.cmake](research/tools/check_bounded_solid.cmake)、[bounded_fixture.py](research/tools/bounded_fixture.py) | 边界 / 兼容 / 往返测试与跨平台一致的UTF-8/LF输入 |
| [bounded_solid_dev.py](research/scripts/bounded_solid_dev.py) | DEV-021各档独立追加历史、全量重建 / 提取 / 故障诊断 |
| [bounded_repeat_dev.py](research/scripts/bounded_repeat_dev.py) | DEV-022固定父包、平衡位置与交替操作顺序 |
| [bounded_repeat_report.py](research/scripts/bounded_repeat_report.py)、[bounded_repeat_preview.py](research/scripts/bounded_repeat_preview.py) | 独立审计、成对统计与表格生成 |

研究工具入口（**不是正式产品 CLI / GUI**）：

```text
seedarc_append4_study bounded-pack seed|explicit FRAME_LIMIT_BYTES INPUT_DIR OUT
seedarc_append4_study bounded-append FRAME_LIMIT_BYTES OLD NEW_DIR OUT
seedarc_append4_study access-profile ARCHIVE INPUT_DIR UNIFORM_SAMPLE_COUNT
seedarc_append4_study payload-fault-profile ARCHIVE INPUT_DIR
```

帧上限字节值：32MiB=`33554432`、64MiB=`67108864`、128MiB=`134217728`；整段对照使用既有1GiB资源上限`1073741824`。当前固定单组、Zstd-9、无字典、off冗余。

实现边界必须保留：

- 只在文件之间分段，一个文件完整位于一个帧中；单文件超所选帧上限明确拒绝。
- 新路径追加保留旧包完整前缀，不重压旧原文，但仍完整读取、复制和写出旧压缩包。
- 旧帧也必须满足所选上限；从已有大帧切到小帧需要显式重建，不能无成本切档。
- 现有v4/v5序列化和旧解码保持；多个帧由已有帧表自描述。帧上限是**Seed之外的显式写入配置**，不能把revision2旧Seed静默解释为包含此参数。
- 仍整树 / 整归档载入内存，提取仍整包读取，尚非完整流式或选择性磁盘I/O。
- 同路径修改、删除、重命名不等于新文件追加，当前不得冒充支持；v5 `partial` / `split`仍拒绝。

## 5. 最新结果与验证范围

DEV-022：同一mixed dev，236,937,106B / 7,993文件；整段 / 32 / 64 / 128MiB四配置四轮，各占一次第1/2/3/4位置，完整重建 / 末批追加先后交替。父包固定为各自第3代，**没有把重复追加的产物再当父包**。

| 配置 | 比整段完整体积 | 四轮同轮读取/解码加速范围 | 重建调用中位 |
| --- | ---: | ---: | ---: |
| 整段参考 | 0 | 1× | 12.021s |
| 32MiB | +0.470% | 2.90–3.33× | 11.906s |
| 64MiB | +0.050% | 2.07–2.33× | 12.052s |
| 128MiB | +0.042% | 1.38–1.56× | 11.939s |

32MiB末批新增18,391,665B，追加中位3.410s，同档完整重建11.906s；最终第4代包比重建大4.699%，仍读取 / 复制40,051,879B旧压缩包。父包准备成本单列，不能称免费生命周期。末批小于所有帧上限，不外推为任意大批次同样加速。

证据与限制：

- DEV-021：Windows Release45/45、Linux Release46/46、相关ASan+UBSan4/4；40包往返、20对Seed / 显式载荷一致。
- DEV-022：Linux Release46/46、13份固定小包SHA跨平台一致，120命令 / 44包独立verify / 32个access-profile通过，全部归档SHA与DEV-021一致。主Agent另审计输入 / 源码 / bundle哈希、轮换顺序和统计。大包验证后由运行器回收，**没有在本机重新解压全部大包**。
- DEV-022没有C++改动、ELF也相同，ASan沿用DEV-021同源4/4，**没有重跑**。显式表示和故障诊断同样沿用同包证据，不是新增覆盖。
- 这些是同dev / 热缓存观测；读取诊断含整包读取、解析、目标帧解码、CRC和字节校验，**未写出目标文件**。不宣称冷磁盘、统计显著性、跨语料保证或压缩吞吐大突破。
- 损坏诊断只覆盖索引 / 头部完好、载荷三个位置翻转bit；不含索引损坏、缺卷或通用恢复。收益来自布局，不是Seed独立优势。

[研究选项表](research/configs/bounded-solid-options-r0.json)保留整段参考、64MiB低额外体积、32MiB偏读取和128MiB手动取舍；没有自动择优、正式用户界面或通用默认。

## 6. 下一步建议

依次推进，开始时在 STATUS 领取任务并预登记方法：

1. **扩数据覆盖**：增加输入类型 / 规模与批次形态，继续隔离dev / holdout；先验证32/64MiB候选的优势是否稳定，不只重复同一已观察数据。
2. **测真实提取与I/O分解**：加入目标文件落盘，分开包读取、索引解析、帧解码、校验 / 写出；随后评估只读取索引和目标帧。不能为跳读而关闭必要的边界 / 校验。
3. **处理追加旧包复制成本**：测不同旧包 / 新批次大小、多轮索引增长和整理成本，再设计有界I/O或增量写入。已有数据不能无提示覆盖。
4. **多codec / 等级的可比曲线**：建立成熟强基线，在相同实际时间预算下比较完整体积；搜索 / 调参双方都收费，不拿事后Oracle当免费在线策略。
5. 据实测推进用户偏好接口与Seed计划选择 / 复用。分段配置、恢复能力和新Seed语义须明确版本化；未冻结内容可演进，已有支持包不能静默失去解码能力。

不要回到“花约87秒只比11.6秒少5KB”的体积搜索主线；那是[DEV-020机制基线](research/experiments/20261124-solid-version-main-001/record.md)，按用户最新目标不主推。

## 7. 远端与复现注意事项

- 使用已安装的 `in-long-task` skill，其文档维护连接 / 凭据方式；不在交接复制私钥内容。默认印度，不自动切香港。
- 用户已允许需要时使用exclusive开放计算资源；**2GiB任务磁盘、时间和日志限制仍保留**。持有任务ID，断连后先查原任务，不重复提交。
- 最新完成任务：`20261124T102611Z-2912f5b3`（DEV-022）；前轮`20261124T092901Z-14334bcf`（DEV-021）。两轮均已fetch / SHA核验后clean自己的work/tmp，结果 / 日志和本机bundle保留。
- 最新bundle：`research/local/india-append/artifacts/20261124T102611Z-2912f5b3.tar.gz`，SHA256=`fcb599ea22e54f5ae23b9911df3924a05f322def934a42367654941dd710297e`，回执同目录。
- 固定工具链 / 依赖仍借用`/home/sc/long-tasks/20261123T123105Z-d5596074/work`；公开Linux源数据仍借用`/home/sc/long-tasks/20261124T051435Z-cef36683/work/data`。这些旧任务有unknown / 中断历史，**不得擅自clean、删除或伪造result.json**。先看本轮bootstrap中的路径 / 哈希断言。
- 固定版本：GCC14.2、Zstd1.5.7、liblzma5.8.4。SSH非交互PATH可能没有CMake；用已确认的任务内工具链路径与环境，不安装全局工具或换低标准。
- skill监控器曾有删除竞态。已有`append_monitor_guard.py`和实验内`task-monitor-patch.py`只修**新准备、尚未启动**的任务副本，检查原SHA和2GiB限制；不改已安装skill，也不直接套到运行任务。
- 跨平台小夹具曾因CMake原生CRLF不一致；现用`bounded_fixture.py`的二进制写入，13份包已一致。**打包源码必须包含该新增辅助文件**，不能只从旧source.zip替换既有成员后遗漏它。

常用本机构建 / 检查（是否需重跑按实际改动决定）：

```text
cmake --build build/dictstudy-check --config Release --target seedarc_append4_study --parallel 2
ctest --test-dir build/dictstudy-check -C Release --output-on-failure --parallel 1
```

独立复算最新结果：

```text
python -B research/scripts/bounded_repeat_report.py research/experiments/20261124-bounded-repeat-main-001/remote/repeat research/datasets/solid-mixed-version-dev-r1/v0.tsv research/experiments/20261124-bounded-solid-main-001/remote/bounded/archives.json <analysis.json>
python -B research/scripts/bounded_repeat_preview.py <analysis.json> <preview.md>
```

## 8. 保留事项

本轮新建临时目录已按任务清理；仓库仍有此前的`.sandbox-test/`残留和`docs/debug.log`，不能顺手删除。GNU Windows旧工具曾有非ASCII canonical路径异常，未解决，不能把MSVC / Linux结果写成所有工具链通过；详见STATUS历史记录。

本交接仅做文档整理与链接 / 差异核对，没有新构建、测试、远端查询或提交。今后以STATUS和对应实验记录的真实证据更新当前状态。
