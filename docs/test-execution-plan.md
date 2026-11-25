# 廉价模型测试执行计划

日期：2026-11-24。代码起点：`d03cdaa`；实际执行必须记录完整 commit 和未提交差异。本任务仅安排实验，不要求修改压缩算法。当前状态统一写入 [STATUS](../STATUS.md)，本文件保存执行约定。

## 可以直接交给执行模型的指令

> 你负责 SeedArc 的 DEV-023 测试执行。先读 AGENTS.md、STATUS.md、HANDOFF.md、本计划和 research/README.md，核对 docs/users_idea 的文件清单与内容哈希。按下面的阶段串行执行，先复算旧结果，再完成新 dev 清单与预登记，最后运行扩展矩阵。允许新增本任务 Python 运行器、分析脚本、配置、清单和实验记录；不要修改 core、归档协议、Seed 语义、C++ 压缩实现或默认参数。远端测试使用已安装 in-long-task skill，先读 skill 再操作。保留任务 ID，断连查原任务，不能重复启动。不擅自提交、推送或清理他人的目录。正确性失败立即停止性能矩阵；证据不足就标为未验证。完成后给出文件链接、实际命令、原始结果、失败和限制，不用预期结果代替实测。

## 研究目标与禁止误读

验证以少量完整体积代价换取读取、追加或损坏隔离优势的取舍。用户选择偏好，不预设某个帧大小永远最佳。单文件提取和补加新文件都是核心场景；本轮先扩展已有诊断覆盖。

- 当前工具固定 Zstd-9，支持文件间分帧和新路径追加；不支持同路径替换、删除或重命名。
- `access-profile` 包含整包读取、解析、目标帧解码与字节校验，没有目标文件落盘。报告名称必须是“读取/解码诊断”，不能写成真实提取耗时。
- 追加仍复制旧压缩包；不重压旧原文不等于零旧包 I/O。
- 载荷故障诊断假设头部与索引完好；不代表索引损坏、缺卷或通用恢复。
- 新布局的收益须同时列同计划显式表示对照，不能归因于 Seed 本身。
- 不继续穷举分组去追逐几 KB；不修改正式产品接口，不增加 Seed 位数，不冻结默认预设。

## 阶段 A 低成本复核

1. 检查 `git status --short` 和当前分支，保留 `.sandbox-test/`、`docs/debug.log` 等既有残留。阅读 [交接](../HANDOFF.md)、[联合目标](../research/ideas/speed-resilience-r1.md)、[有界固实](../research/ideas/bounded-solid-r0.md)及 DEV-022 的 record.md。
2. 在 STATUS 的 DEV-023 行写入执行者、实际范围与状态。新实验使用不重名的 `research/experiments/<日期>-bounded-expand-<执行者>-001/`，不要修改旧实验事实。
3. 把以下复算输出写入新实验目录；代码块里的 `NEW_RUN` 是占位符，先替换为实际新目录，不得覆盖原 analysis.json。

```text
python -B research/scripts/bounded_repeat_report.py research/experiments/20261124-bounded-repeat-main-001/remote/repeat research/datasets/solid-mixed-version-dev-r1/v0.tsv research/experiments/20261124-bounded-solid-main-001/remote/bounded/archives.json NEW_RUN/recomputed.json
python -B research/scripts/bounded_repeat_preview.py NEW_RUN/recomputed.json NEW_RUN/recomputed.md
```

4. 核对 120 命令、44 归档、32 读取诊断及原表指标。比较数值与断言，输出文件不必与旧分析文件逐字节相同。失败先查输入/脚本版本，不能删断言让其通过。
5. 不为此重跑旧大规模 DEV-022。已有二进制的使用须核对源码、配置和哈希；无法证明相同就重新 Release 构建，并在新远端环境跑 CTest。测试数量随版本变化，不能把“必须等于 46”当作唯一验收标准。

## 阶段 B 预登记新矩阵

先保存配置与清单，再观察压缩结果。以下都是已有公开 dev 的分层扩展，**不是 holdout**。新增独立数据来源和正式留出评估交回主模型另行登记。

| 项目 | 本轮约定 |
| --- | --- |
| 输入来源 | 已有 Linux 清单中的源码文件；已有 enwik8/Silesia dev 中的文本类文件，分别成树，保留来源命名空间 |
| 大小 | 每类目标 128–256 MiB；若现有来源不足，使用全部符合条件文件并记录实际大小，不复制填充、不任意切片凑数 |
| 单文件上限 | 32 MiB；按大小在测量前筛选并列出排除清单，不能因压缩不好而排除 |
| 路径与顺序 | 清单固定相对路径、字节数、SHA256；按规范化相对路径确定性排序。拒绝路径逃逸、符号链接和重复路径 |
| 新批次 | 两种独立历史：新增约最终原文字节的 10% 和 40%；整文件分配，记录实际比例与规则 |
| 帧大小 | 1 GiB 参考、32 MiB、64 MiB、128 MiB，四档全部保留，不根据结果减掉差档 |
| 固定配置 | 单组、Zstd-9、无字典、off 冗余；显式表示与 Seed 使用相同具体计划 |
| 执行方式 | 一次一条压缩命令；与旧观测一致的单 CPU 亲和性，记录实际线程/CPU/RSS。允许服务器全资源不等于同时跑多份测速 |
| 缓存 | 延续预热输入及 verify 后读取诊断；明确为热缓存。不执行系统级 drop_caches |
| 重复顺序 | 每种输入、批次形态四轮；四帧配置循环移位，各占一次第1至第4位置；重建/追加先后逐轮交替 |
| 目标文件 | 每个最终输入树统一取相对路径排序下12个均匀样本及最大文件，记录实际路径与去重后数量；所有帧档复用同一集合 |

批次分配使用与压缩结果无关的固定路径哈希顺序，累计文件字节至最接近目标比例，写明算法和固定版本。两种历史各自建立不可变父包；最终文件树相同。前两轮的输出不能充当后两轮父包。本轮只做一次新增批次，不冒充 DEV-022 的四代生命周期。

若合格文本输入不足以超过 64 MiB，仍可记录为小规模对照，但必须明确部分帧档可能产生相同布局。不要因此自动扩大下载或伪造“跨大规模”结论。

先完成“源码类＋10%新增”的四轮并审计，正确后自动推进其余组合。预计主矩阵为 2 输入类 × 2 批次比例 × 4 帧档 × 4 轮 × 2 操作 = 128 次编码；父包准备、verify、显式对照与故障诊断另列，不混进这128次。若输入不足或预算受限，完整记录已完成单元，不能报为全矩阵通过。

## 阶段 C 运行器与执行

可参考 [bounded_repeat_dev.py](../research/scripts/bounded_repeat_dev.py)、[bounded_solid_dev.py](../research/scripts/bounded_solid_dev.py) 的计量、验证与目录安全代码。现有 repeat 运行器绑定旧四批数据、旧代数和 DEV-021 参考 SHA，**不能直接换新 manifest 后调用，也不能删除旧断言来复用**。新增独立脚本与配置，旧证据仍可复算。

已存在的研究命令如下，所有输出路径必须全新且位于本任务目录：

```text
seedarc_append4_study bounded-pack seed FRAME_CAP PARENT_INPUT PARENT_ARCHIVE
seedarc_append4_study bounded-pack explicit FRAME_CAP PARENT_INPUT EXPLICIT_ARCHIVE
seedarc_append4_study payload-compare PARENT_ARCHIVE EXPLICIT_ARCHIVE
seedarc_append4_study bounded-append FRAME_CAP PARENT_ARCHIVE ADDED_INPUT APPENDED_ARCHIVE
seedarc_append4_study bounded-pack seed FRAME_CAP FINAL_INPUT REBUILT_ARCHIVE
seedarc_append4_study verify APPENDED_ARCHIVE FINAL_INPUT
seedarc_append4_study verify REBUILT_ARCHIVE FINAL_INPUT
seedarc_append4_study access-profile APPENDED_ARCHIVE FINAL_INPUT 12
seedarc_append4_study payload-fault-profile APPENDED_ARCHIVE FINAL_INPUT
```

新脚本先用固定小树 smoke：包含空文件、中文文件名/目录、多个小文件、恰好等于测试帧上限的文件及超上限文件。大于上限须明确失败且没有可误认为成功的输出；正常样例逐字节 verify。使用二进制写入或 [bounded_fixture.py](../research/tools/bounded_fixture.py) 约定的 UTF-8/LF，不依赖 Windows 原生文本换行。

每次编码/追加后必须 verify。检查完整包物理字节与指标一致、最大帧原文字节不超上限、父包 SHA 不变、旧包是追加包前缀、旧原文重压为零。重复相同输入/配置应有相同归档 SHA；首次新配置的 SHA 来自本轮，不能伪造旧参考。

父包准备单列耗时；每个输入类/帧档至少核验一组 Seed/显式载荷相同。每种输入/批次/帧档首次产物做现有 payload-fault-profile，后续同 SHA 重复包可复用该诊断并明确标注。诊断若报接受错误字节立即停测；无可恢复数据也如实保存。

每条命令保存 argv、退出码、stdout/stderr、进程墙钟、工具原始 JSON、包大小/SHA、输入/脚本/二进制哈希。工具内编码时间与进程墙钟分列；verify、预热和父包准备不塞进编码项，也不能从生命周期总成本中消失。OS 实际磁盘读取量未测则写未测，不能用逻辑包大小冒充。

## 印度服务器与预算

执行时读取 `in-long-task` skill；连接、上传、preflight、任务句柄、取回与清理由 skill 的真实接口决定。本计划不复制凭据，也不虚构命令参数。

- 先查是否已有任务运行、固定工具链/数据是否仍在、文件哈希和剩余磁盘是否满足。交接中的旧路径只是线索，不能当实时保证。
- 同主机只跑一个本任务性能作业。exclusive 计算资源已有用户授权；磁盘 2 GiB、日志和时间上限仍按 skill 保留，不能关闭监控器。
- 每条性能命令超时上限 1800 秒，总矩阵预算先登记，默认不超过90分钟；skill 更紧的上限优先。预计超出时保存完整已完成单元并交回，不拆成重复任务绕过限额。
- 输入、父包、全量重建包、追加包、构建与结果一起算峰值磁盘；逐单元运行，空间估算不足先缩减预登记规模，不边测边偷偷改输入。
- 新源码包包含新增 helper；保存源码清单与哈希。新运行器引用的固定版本与数据哈希必须断言，不能自动换依赖。
- 断连或超时先查原任务状态。未知状态不重启，不删除任务结果来“恢复”。旧任务依赖目录和他人目录不清理。
- 结果取回、bundle SHA核验并验证指标后，才按 skill 清理本任务可再生 work/tmp；必要日志与原始指标保留，大文件留在忽略的 research/local。

## 阶段 D 分析与交付

按输入类、实际新增比例、帧档分别输出以下指标，不把不同语料混成一个平均数：

1. 完整体积及相对同轮1GiB参考的增幅，四轮墙钟的最小/中位/最大及同轮成对倍率。
2. 读取/解码诊断的样本路径、解码量、样本中位与p95、各轮范围；13个左右样本的p95不是生产尾延迟保证。
3. 同档追加对比全量重建：墙钟、最终完整体积、旧包读取/复制字节、旧原文重压字节、父包准备成本。
4. 故障检测、错误字节接受数、逐字节核验可恢复量及不可恢复量；仅对实际诊断条件下结论。
5. 全部失败/中止、排除输入、配置漂移、缺失指标和预算消耗。失败行保留，不用平均数吞掉。

独立分析脚本检查预登记矩阵数量、包 SHA 重复性、父包不可变、样本一致性及所有 verify 结果。最终汇总列“大小—重建时间—追加时间—读取诊断—恢复范围”取舍；可以标出各项不劣且至少一项更好的支配关系，不能替用户选通用默认。

交付清单：新实验 record.md、预登记配置、数据清单/哈希、原始JSONL与必要日志、可重跑分析脚本、汇总表、远端任务ID/退出状态/bundle哈希、STATUS更新。主模型审计前写“执行者报告通过”，不能自称已获主模型独立验收。

## 停止条件与后续边界

正确性/安全断言失败、sanitizer报错、父包改变、输入哈希不符时立即停止性能扩展并保留证据。同一问题三次修复仍无实质进展停止盲试；权限、认证、未知任务状态更早交回。正常波动或没有收益不是程序失败，完成预登记重复即可，不靠追加试验筛选好看结果。

以下另立后续开发任务：真实单文件落盘与I/O分解、选择性磁盘读取、多轮追加/整理生命周期、多codec同实际时间曲线、索引损坏/缺卷恢复、产品偏好接口。它们需要实现或设计审查，廉价执行模型不要为凑测试覆盖自行扩协议。
