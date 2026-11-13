# SeedArc 当前状态与协作交接

更新日期：2026-11-12。此文件是当前进度入口；[开发计划](docs/development-plan.md) 保存长期路线，[research](research/README.md) 保存研究证据。

## 当前基线

- 2026-11-12 提交整理：补充 DEV-013 已出现的工作区产物，尚未完成该研究阶段。r1 实验记录已更正 dev / test 内容重叠（614 个 test 文件）；新增 [r2 语料说明](research/datasets/b2r5-corpus-r2.md)、JSON / TSV 与生成脚本。此次实际检查：`b2_corpus_r2.py --check` 内容交集为 0，六份清单哈希与 JSON 一致，Python 语法和 JSON 解析通过。`core/codec/dictionary.*` 仅保存为未验证研究草稿，尚未纳入 CMake、编译或往返测试，不属于已完成能力。未重新运行构建、CTest、ASan 或 CLion；调试日志 `docs/debug.log` 不纳入提交。最新基线提交已为 `631cb39`（此前“未提交推送”属于历史记录）；本条对应的后续提交以 Git 记录为准。

- 最新 B2-r5 P1 独立评估已完成（DEV-012）：adaptive-file-r1 已冻结（[配置](research/configs/adaptive-file-r1.frozen.json)，默认参数有回归测试）；新建本机独立语料 [b2r5-corpus-r1](research/datasets/b2r5-corpus-r1.md)（10 类，dev / test 各约 89 MB，按路径哈希预先划分，没有下载外部数据）。在 test 上 350/350 往返通过：Adaptive-r1 各档逐文件都不比 Zstd-3 大，但无界档（−7.45 %，14.5 s）被 Fixed LZMA2-6（−7.95 %，13.6 s）在大小和速度上同时压过；256 KiB / 1 MiB 档几乎全部耗尽预算（−0.09 % / −0.66 %）；Oracle 只比 LZMA2-6 再小 0.025 %；B-r1 seed0（有 Seed、无搜索）+26.1 %。消融 G5（有 Seed + 预算搜索）缺项，本轮不对 Seed 的独立价值下结论。见 [run 记录](research/experiments/20261112-b2-independent-main-001/record.md)、[消融设计](research/ideas/seed-ablation-r1.md)。另：其余测试夹具清理的 `remove_all` 已统一换成有界的 `tests/test_fixture.hpp`（原 seed_test_fixture.hpp 提升为通用）。

- 最新 B2-r5 P0 已完成（DEV-010）：P0-A 复现 r4 的 8 个原方案与 Oracle 共 45 组归档 SHA256 一致；P0-B 新增研究层 Adaptive-file 选择器 `adaptive-file-r1`（默认 Zstd-3、首中尾抽样、显式试编码预算、确定性回退，只输出既有 BlockPlan，归档 / 编解码 / 索引不变，metadata_bytes=0）；P0-C 统一预算对照：五组合计 Zstd-3 1,192,886 B / 60 ms，Adaptive ≥256 KiB 1,191,683 B（−1,203 B）/ 72–132 ms，Oracle 1,191,681 B / 198 ms，LZMA2-6 −320 B / 191 ms。收益全部来自 source 的一个文件；各档都不比 Zstd-3 大。195/195 往返通过，Release 36/36。仅开发样本，未调参，下一步 P1 独立语料。见 [run 记录](research/experiments/20261112-b2-adaptive-main-001/record.md)与[预登记规则](research/ideas/seed-adaptive-file-r5.md)。

- 最新 B2-r4 P0 逐块诊断与简单基线已完成（DEV-009）：text / mixed 上 B0（b-global）比固定 Zstd-3 多出的 65,594 / 66,903 B，99.78% 来自 B-r1 与内容无关地把块计划为 Store（不尝试压缩），其余为 LZMA2-6 每块多 72 B；等级、头部、索引贡献为 0，块边界一致。B1-fixed-zstd3 与 B2-file-codec 在两组均回收全部损失；source 上 B2-file-codec 8,298 B（单样本 3 配置 oracle，仅比 lzma6 小 6 B）。150/150 往返通过，8 个原方案 40 组归档与 B2-r3 逐字节同哈希；Release 全量 35/35。见 [run 记录](research/experiments/20261112-b2-diagnose-main-001/record.md)与[假设](research/ideas/seed-codec-allocation-r4.md)。未改算法、未扩大搜索、未冻结格式；Seed / 覆盖元数据仍未序列化（缺项）。

- 最新B2-r3具体计划压缩接入已验证：生产core消费具体计划，研究层解释B与覆盖；归档格式不变。Release/ASan全量各35/35、Clang定向5/5，120/120磁盘往返与双向旧包兼容通过。集中CLion9文件无ERROR，保留2条公开stop_token值传递建议。见 [验证](docs/b2-compression-validation.md)与[基线](research/experiments/20261112-b2-compression-main-001/preview.md)。仅源码样本文件覆盖减667字节，其余4类不变；未冻结Seed或宣称普遍收益。

- 最新 B2-r2 计划层契约已验证：精确路径匹配、目标删除 / 重命名后旧覆盖失效、叠加等级3→4→6→4、空文件 / 无目标 Codec 无变化、10000块单文件测试；Release34/34、ASan定向4/4、Clang定向3/3，7文件IDE无WARN/ERROR。见 [记录](docs/file-contract-validation.md)与[对照](research/experiments/20261112-file-contract-main-001/preview.md)。Seed 编码仍未冻结。

- 最新研究原型 overlay-r1 已验证：固定3文件12块，全局Zstd修改影响5块、文件1覆盖影响块5/6、清除恢复当前默认；保留A/B-r1与归档格式。Release全量33/33，定向ASan/Clang及IDE运行通过；待用户评估文件粒度。见 [本轮记录](docs/file-scope-validation.md)。

- M0 工程骨架已完成：C++23 核心库、CLI、CMake presets 与 CTest。
- M2 **P1 / P2 修复后独立复验通过，阶段完成**（代码 `4c547b2`）：主线程重建并执行 MSVC Release 25/25、ASan Release 25/25、CLion Debug 定向验收 exitCode=0；四个修复相关文件 IDE 诊断完整返回无 ERROR。原实现者七套配置证据保留。发布树保留已确认的私有 DACL，受信同 SID / SYSTEM / 管理员在最终复核后修改不在保证内；格式仍实验性、未冻结。见 [验收记录](docs/m2-acceptance.md)“第二轮独立复验”。
- M3 **独立验收通过，阶段完成**（DEV-004 / ACC-003，2026-11-12）：按块 Zstandard / LZMA2 编码与 Store 回退，依赖由 vcpkg manifest 钉住版本与 baseline；本轮独立 Release / ASan 各 30/30、CLion 13/14 文件完整返回无 ERROR（另 1 文件由用户人工补查仅拼写提示）；随后修复默认 Debug profile，IDE CLI 与 codec 测试均成功；原实现者七套配置（含 ASan）30/30，Fuzz 短跑无崩溃；CLion 诊断（clang-ucrt64-debug，errorsOnly=false）覆盖 13 个改动文件，无 ERROR，修复 2 条有效 WARN 后三套配置复测 30/30。见 [M3 记录](docs/m3-validation.md)。
- M1 已完成并由用户验收：实验头部 / 块范围 / 路径校验和安全 I/O 原语，含卷身份、累计深度 / 路径预算、共享模式调整与故障注入测试；头部 / 路径 libFuzzer 短跑无崩溃。六套配置与 MSVC ASan 均 18/18、无警告；CLion 诊断已完成（修复 1 个 ERROR）。未接 CLI，见 [接续记录](docs/m1-validation.md)与[候选 ADR](docs/decisions/0001-experimental-container-and-output.md)。
- 已有项目规范、开发路线、GPL-3.0-or-later 声明及正文，Git 远端 origin 为 https://github.com/vusiif/SeedArc（main 已推送首次提交 `fbd3fb8`）。
- 已完成 M4-A 第一轮独立研究原型：A/B-r1 输出计划与字段差异，尚未接入归档、尚未由用户评估语义。小型 M3 合成基线 27/27 内容哈希往返通过；Release / ASan 各 32/32，Clang 定向 2/2，6 文件 CLion 完整诊断无 WARN / ERROR。见 [M4-A 记录](docs/m4a-validation.md)；没有 Seed 搜索收益或独创性结论。
- third_party 已保存有来源许可与适配记录的 SplitMix64 单步片段，仅用于种子研究；轻量 Qt GUI 改为按用户人工交互测试需要启动；Actions 按用户最新要求延后。

## 里程碑

| ID | 目标 | 状态 | 完成门槛 / 证据 |
| --- | --- | --- | --- |
| M0 | C++23 / CMake / CLI / CTest 骨架 | 完成 | 三套工具链 Debug / Release 各 13/13，另有 MSVC ASan 13/13；见验证记录 |
| M1 | 实验格式与安全 I/O | 完成（用户 2026-11-11 验收） | 六套配置 + ASan 18/18，故障注入与 Fuzz 短跑通过，CLion 诊断无剩余 ERROR；已知限制：真实 SUBST 未测、Fuzz 仅短跑；见 M1 记录 |
| M2 | Store 归档往返 | 完成（2026-11-11 独立复验通过） | 代码 4c547b2；独立 Release / ASan 各 25/25，IDE Debug 定向回归通过；保留已确认 DACL 与信任边界，见 [验收记录](docs/m2-acceptance.md) |
| M3 | Zstd / LZMA2 | 完成（2026-11-12 独立验收） | 本轮 Release / ASan 各 30/30、IDE 13/14 完整诊断无 ERROR，限制见 M3 记录；原实现者 七套配置 + ASan 30/30，archive Fuzz 180 s 无崩溃；CLion 诊断无 ERROR、2 条 WARN 已修；见 [M3 记录](docs/m3-validation.md)、[ADR-0003](docs/decisions/0003-m3-zstd-lzma2-codecs.md) |
| M4 | 种子设计探索、确定性与 MVP | B2-r3真实压缩实验已验证，待用户评估与继续选型 | 先比较候选并由用户评估语义，再选择实验方案接入；固定向量、归档复现、动态检查与基线 |
| UI-1 | 按人工交互测试需要启动的轻量 Qt GUI | 延后，按需启动 | 基础操作、往返与错误显示、界面响应，核心无 Qt 依赖 |
| M5 | 公平基准与预算搜索 | 待开始 | 可复现实验、取消与最优结果保留 |
| UI-2 | M5 后的搜索界面 | 待开始 | 预算、真实搜索曲线与结果对比 |
| M6 | 结构化研究与产品化 | 待开始 | 按实验收益及发布门槛逐项推进 |

## 任务表

状态：待开始 / 进行中 / 待验收 / 完成 / 阻塞。领取时填写真实负责人和写入范围；完成后保留精简证据。

| 任务 ID | 任务 | 负责人 | 写入范围 | 状态 | 验收 / 证据 |
| --- | --- | --- | --- | --- | --- |
| DOC-001 | 研究归档及协作规范 | 本次主 Agent | AGENTS、STATUS、README、研究目录及相关流程文档 | 完成 | 本地链接、JSON 模板与归档结构检查通过；仅文档工作 |
| DOC-002 | 第三方源码目录与 UI 计划同步 | 本次主 Agent | third_party/README、AGENTS、README、STATUS、开发计划与架构文档 | 完成 | 21 份 Markdown 本地链接检查通过，third_party 文档未被项目规则忽略；无源码引入或构建测试 |
| DOC-003 | 用户想法更新检查与设计输入 | 主 Agent | AGENTS、STATUS、README、开发计划与流程文档 | 完成 | 已读两份原稿，记录哈希与影响；23 份 Markdown 本地链接及原稿哈希核对通过，原稿未改动；仅文档工作 |
| IDE-001 | CLion MCP 诊断接入与规范 | 主 Agent | AGENTS、开发说明、STATUS，必要的相关代码修正 | 完成 | 超时根因已定位（Nova 只分析可见编辑器；窗口失焦后不出结果），方法写入开发说明；17 个源文件 / 头文件取得 errorsOnly=false 诊断，修复 1 个 ERROR（越界枚举转换），其余仅风格提示；见 [M1 记录](docs/m1-validation.md#clion-诊断第四轮) |
| DEV-001 | M0 工程骨架 | 主 Agent | CMake 配置、core、cli、tests、构建说明与 STATUS | 完成 | 六套配置各 13/13，ASan 13/13、格式及静态检查通过；见 [M0 记录](docs/m0-validation.md) |
| REV-001 | 外部审查阶段 1 修复 | 主 Agent | README、开发计划、验证记录、ADR、CLI 提示、error.hpp、archive.hpp、safe_output、相关测试、ADR-0002 | 待验收 | 审查原文 [docs/review_results/](docs/review_results/SeedArc_项目代码审查.md) 未改动。采纳：状态统一到 STATUS；CLI 提示去里程碑化；新增 `ErrorCode::cancelled`；`max_blocks` 1000000→262144 并加 `limits_are_coherent` 与边界测试；M2 约束写入 [ADR-0002](docs/decisions/0002-m2-store-archive-constraints.md)。延后：流式写出 / 事务 / 完整索引（M2）、CI（需用户决定，Linux 需先定 POSIX 路径规则）、CRC32C 加速（先测量）。六套 + ASan 18/18、format / tidy 通过；CLion（前台、逐个打开）复查 6 个改动文件无 ERROR，仅风格提示；safe_output.cpp 两条 `Clang-Tidy: Potential memory leak`（第 248 行 `new (storage.data()) FILE_RENAME_INFO{}` 及其在析构处的报告点）判为误报：placement new 不分配堆内存，对象可平凡析构，存储归 `std::vector` 所有；未压制检查 |
| DEV-003 | M2 Store 归档端到端 | 原实现 / 修复会话；本轮主 Agent 独立验收 | core、cli、tests、fuzz、ADR-0002、M2 记录、README、STATUS | 完成 | P1 / P2 独立复验通过：Release / ASan 各 25/25，IDE Debug 定向回归成功，4 文件完整诊断无 ERROR；本轮未改实现、未提交推送；[验收记录](docs/m2-acceptance.md) |
| DEV-012 | 冻结 r1、独立语料、B2-r5 P1 评估、Seed 消融设计、夹具清理统一 | 主 Agent（B2 协调会话委派） | research/configs/adaptive-file-r1.frozen.json、research/datasets/b2r5-corpus-r1*、research/scripts/b2_corpus.py / b2_independent.py 与 README、research/ideas/seed-ablation-r1.md、seed-adaptive-file-r5.md、新 run 20261112-b2-independent-main-001、tests/test_fixture.hpp（由 seed_test_fixture.hpp 改名并扩充）、tests/{seed_adaptive,seed_archive,store_archive,codec,store_acceptance}_tests.cpp、STATUS | 完成，待用户评估 | 350/350 往返、同组哈希一致；MSVC Release / ASan、GCC Debug / Release、Clang Debug / Release 全量各 36/36，构建 0 警告；GCC Debug 与 Clang Debug 下改动的 5 个测试各重复 50 次，0 失败、0 超时、无残留夹具；CLion 6/6 文件 0 ERROR / 0 WARN（store_archive_tests.cpp 在前台补查） |
| DEV-011 | `seed.archive_integration` 偶发失败排查 | 主 Agent（B2 协调会话委派，用户同意） | tests/seed_archive_tests.cpp、tests/seed_adaptive_tests.cpp、新 tests/seed_test_fixture.hpp、docs/b2-compression-validation.md、STATUS | 完成 | 根因：外部组件在删除后短暂列出的“原名大写 + .tmp”条目干扰了测试计数，另有 libstdc++ remove_all 死循环；实现无残留、无句柄泄漏。Release/ASan 全量 36/36；重复运行 Release 1000、ASan 200、Clang 1500 次 0 失败；见验证记录 DEV-011 节 |
| DEV-010 | B2-r5 P0 可复现基线、Adaptive-file 与统一预算对照 | 主 Agent（B2 协调会话委派） | core/seed/adaptive_file.*、core/CMakeLists 一行、research/tools/seed_archive.cpp（r5 输出、adaptive-file / oracle-file）、tests/seed_adaptive_tests.cpp 与 tests/CMakeLists、research/scripts/b2_adaptive.py 与 README、research/ideas/seed-adaptive-file-r5.md、新 run 20261112-b2-adaptive-main-001、STATUS | 完成 P0，待用户评估 | Release 36/36；195/195 往返，P0-A 45 组同哈希；ASan 36/36，Clang 定向通过（修 1 条有效警告后 0 警告），最终二进制复核 35 例一致；CLion 6 文件 0 ERROR，2 条 WARN 已修；既有 seed.archive_integration 偶发失败已由 DEV-011 修复 |
| DEV-009 | B2-r4 P0 逐块诊断与简单分配基线 | 主 Agent（B2 协调会话委派） | research/tools/seed_archive.cpp（r4 输出与 file-plan 模式）、research/scripts/b2_diagnose.py 与 README、research/ideas/seed-codec-allocation-r4.md、新 run 20261112-b2-diagnose-main-001、STATUS | 完成 P0，待用户评估 | Release 35/35；150/150 往返、对账 50/50、块边界一致、原方案 40 组归档与 B2-r3 同哈希；ASan 35/35，Clang 定向构建 0 警告；file-plan 负面 11 项；格式化后复核；CLion seed_archive.cpp（errorsOnly=false）0 ERROR / 0 WARN，仅 INFO |
| DEV-008 | B2-r3 具体计划压缩接入与小型基线 | 主 Agent | core内部计划入口、seed研究适配、工具 / 测试 / CMake、规则 / 新实验与状态 | 集成与小型基线通过，待用户评估 | Release/ASan各35/35、Clang5/5，修正后定向回归通过；120次磁盘往返、双向兼容、5例跨工具链一致；9文件CLion无ERROR，限制见 [记录](docs/b2-compression-validation.md) |
| DEV-007 | B2-r2 文件身份与叠加契约 | 主 Agent | core/seed/path_overrides、测试 / 研究工具 / 规则与新实验、相关 CMake、STATUS | 集成验证通过，待用户评估 | Release34/34、最终定向4/4；ASan定向4/4及最终新测试1/1，Clang3/3；7文件IDE无WARN/ERROR，IDE测试与工具退出0；见 [记录](docs/file-contract-validation.md) |
| DEV-006 | 文件级等级覆盖最小原型 | 主 Agent；子任务 m4_seed_prototype | core/seed/file_overrides、研究工具、相关 CMake / 测试、规则与本轮实验 / 验证记录 | 集成验证通过，待用户评估 | Release 33/33，IDE 修正后定向3/3、ASan定向3/3、Clang定向2/2；4组跨编译器输出一致，4文件IDE无ERROR（保留1条已核实include提示）；不改A/B或归档，见 [验证记录](docs/file-scope-validation.md) |
| DEV-005 | M4-A 种子候选原型与小型 M3 基线 | 主 Agent；子任务 m4_seed_prototype | core/seed、research/tools、相关 CMake / 测试、third_party/splitmix64；research 配置 / 脚本 / 本轮实验 / 文档 | 原型通过，待用户评估 | 主线程 Release / ASan 各 32/32、Clang 定向 2/2，跨工具链 4 组输出字节一致；6 文件完整 IDE 诊断无 WARN / ERROR，IDE 两个目标运行成功；基线 27/27，通过范围与限制见 [M4-A 记录](docs/m4a-validation.md) |
| PLAN-004 | Seed 探索路线与优先级调整 | 主 Agent | 开发计划、架构、research/ideas、STATUS | 完成（仅设计记录） | 位宽 / 字段 / 映射未冻结；新增表示与映射替换边界、候选修订及旧包解码约束，加入 Agent 规范；A/B/C 候选与用户评估关口，GUI 按需、Actions 延后；未实施算法或运行实验，见 [设计探索](research/ideas/seed-design-study.md) |
| IDE-002 | CLion 默认 Debug 依赖配置修复 | 主 Agent | 本地 CLion Debug profile、开发说明、M3 记录、STATUS | 完成 | MCP 重新加载成功；确认 MSVC / C++23 模式与 vcpkg 依赖；IDE CLI 版本、codec 测试均 exitCode=0，CMake 文件完整诊断无问题，见 M3 记录末节 |
| ACC-003 | M3 独立验收 | 本轮主 Agent；只读子审查 m3_review | STATUS、M3 验收记录；不改原实现 | 完成 | Release / ASan 各 30/30；format / diff 检查通过，IDE 13/14 文件完整无 ERROR；1 文件人工补查仅拼写提示；IDE 运行已在 IDE-002 补齐，详见 [M3 记录](docs/m3-validation.md#独立验收2026-11-12) |
| DEV-004 | M3 Zstd / LZMA2 块编码 | 主 Agent | vcpkg manifest、CMakePresets、core/codec、archive / store_archive / safe_output、CLI、tests、fuzz 语料、ADR-0003、M3 记录、README、开发说明、STATUS | 完成（独立验收通过） | zstd 1.5.7 / liblzma 5.8.4 经 vcpkg 钉住；七套配置 + ASan 30/30，format / tidy 通过，archive Fuzz 180 s 无崩溃；CLion 诊断 13 个文件无 ERROR，修 2 条 WARN 后 MSVC Release / Clang Debug / GCC Release 复测 30/30；[M3 记录](docs/m3-validation.md) |
| DEV-014 | Linux L1：WSL 构建、presets、可运行测试、ASan + UBSan、短时 Fuzz | DEV-014 子会话（协调会话委派） | WSL 克隆分支 `linux-l1`：CMakePresets、根 / tests / fuzz CMake、core/filesystem/archive_path.cpp、tests/{codec,path,seed_path_override}_tests.cpp、check_cli.cmake、docs/development.md §7、新 docs/linux-l1-validation.md、docs/posix-filesystem-design.md、STATUS | 待验收（未推送 / 未合并） | GCC / Clang Debug / Release 与 ASan+UBSan 各 27/27、0 警告，11 项 Windows-only 测试 DISABLED 并列原因；3 个 fuzz 目标各 180 s 0 崩溃；Linux / MSVC 内存编码 21 个哈希一致（一次观察）；MSVC Release 36/36；CLion 未查，见 [L1 记录](docs/linux-l1-validation.md) |
| DEV-002 | M1 实验格式与安全 I/O 设计 | 主 Agent；两个已结束子任务 | 候选 ADR、archive / filesystem 模块、tests、验证记录 | 完成 | 用户 2026-11-11 验收。error 5（沙盒 ACL）定位、提交重命名修复；卷身份 / 深度预算 / 共享模式 / 故障注入 / Fuzz 完成；六套 + ASan 18/18，format / tidy 通过；CLion 诊断完成并修 1 个 ERROR；真实 SUBST 未测；[M1 交接](docs/m1-validation.md) |

## 验证与证据

- 文档及模板检查：本地 Markdown 链接目标存在、JSON 模板可解析、研究归档目录齐备。
- M0：MSVC 19.51、GCC 16.2、Clang 22.1 的 Debug / Release 编译、链接、运行及 CTest 全部通过，每套 13 项；MSVC Release ASan 13/13，clang-format 与 clang-tidy 的分析器检查通过。命令、范围和日志位置见 [M0 验证记录](docs/m0-validation.md)。
- M2（2026-11-11）：七套配置全量重建后 CTest 24/24、构建 0 警告、MSVC ASan 无报告；`clang-format` / `clang-tidy` 通过；64 MiB 单文件往返峰值工作集增长约 1.4 MiB（断言上界 16 MiB）；Fuzz 短跑结果、命令、日志位置和未验证项见 [M2 记录](docs/m2-validation.md)。本轮未取得 CLion 诊断（clion MCP 服务 ECONNREFUSED）。
- M4-A：研究运行包括 20261112-m3-baseline-main-001、20261112-seed-plans-main-001，以及文件覆盖观察 20261112-file-scope-main-001、行为契约观察 20261112-file-contract-main-001、真实压缩首轮 20261112-b2-compression-main-001；前者为小型单块合成基线，后者仅观察拟议计划。已运行小型磁盘源码/自制样本压缩基线；尚未运行独立代表性真实语料 / Seed搜索评估、持续Fuzz。
- 本轮 DEV-008 由主 Agent 实现与验证，未启动新子任务；前轮子任务 m4_seed_prototype 已交还写权限；无运行中的构建 / 研究任务。实验事实与源码快照见上述 run ID。
- CLion MCP：通过客户端已有配置发现回环服务（不硬编码连接地址），initialize 确认为 CLion MCP Server 2026.3.0.1；实际调用 tools/list、get_run_configurations、get_compiler_info、open_file_in_editor、get_all_open_file_paths 和 get_file_problems。确认 SeedArc 项目、MSVC Debug、`-std:c++latest` 和 `/Zc:__cplusplus`；活动编辑器为 cli/main.cpp。当前会话无原生 JetBrains 工具入口，使用已配置服务的实际 MCP JSON-RPC 调用，不以命令行分析冒充 IDE。
- M1 第三轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 18/18（新增 fuzz 语料重放 2 项）；`msvc-fuzz` libFuzzer header 3065144 次 / path 576342 次（各 151 s）无崩溃；format / tidy（13 个 .cpp）通过；CLion 仍 timedOut。用户想法 SHA256 复核未变。
- M1 第二轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 16/16；clang-format / clang-tidy（全部 10 个 .cpp）通过；CLion get_file_problems 对 4 个文件仍 timedOut。详见 [M1 记录](docs/m1-validation.md)。用户想法两份 SHA256 复核未变。
- CLion 检查：早期超时的根因已在 IDE-001 定位（只分析可见编辑器、窗口需在前台），方法见[开发说明](docs/development.md#clion-mcp-检查)。返回 `timedOut=true` 的空结果不代表没有诊断。

## 阻塞、限制与下一步

- DEV-012 下一步建议（尚未实施）：在 **dev** 划分上做 adaptive-file-r2：把 LZMA2-6 纳入默认或候选顺序、重新审视 16 KiB 小文件阈值（terminfo 上 LZMA2 能省 11 %，r1 为 0）、用廉价特征跳过已压缩数据（compressed-packages 试编码 8.6 MB 一字节未省）、改按时间计预算，最后再在 test 上做一次性评估。G5 搜索器、跨文件字典 / 固实压缩（P1-1）仍待立项。CLion 对 tests/store_archive_tests.cpp 的诊断已在前台补查，只有 INFO。本轮未提交推送。

- DEV-011 已排查并修复 `seed.archive_integration` 偶发失败（DEV-010 发现）：**测试观测问题，不是实现问题**。在同一次遍历中计数并记名的诊断下，Release 300 次失败 22 次，全部是外部组件在文件删除约 0.7 ms 后短暂列出、随后约 0.2 ms 消失的“原名大写 + .tmp”条目（`FIXED-*.SARC.tmp`）。这些条目打开即 NOT_FOUND，与被删对象的 FileId 不同；所删的是测试自己刚用 `fs::remove` 删掉的已提交对照归档，失败写入从未残留。具体是哪个外部组件未查明（Defender 未运行；Windhawk 已注入但只有时钟 mod；`fltmc` 无权限）。验证过程中另发现 libstdc++ 16.2 的 `remove_all` 遇到同类正在消失的条目时死循环（Clang 下 CTest 超时；gdb 栈为 `remove_all→__erase→remove→symlink_status`，卡住的路径是失败写入临时名对应的外部条目；外部可 rmdir，证明没有泄漏句柄）。改动只涉及测试：失败写入改到专用目录并按对象核对（删除挂起仍算残留，并有自检）；清理改用有界的 `seed_test::remove_tree`；安全扫描对已消失的名字视为安全。验证：Release / ASan 全量各 36/36；`seed.archive_integration` 在 Release 200+300 次、ASan 200 次、Clang 200+300 次 CTest 重复中 0 失败，另有直接运行 Release 500 次、Clang 2×500 次 0 失败 / 0 卡死 / 0 残留；CLion 3 个测试文件 0 ERROR，新增 1 条 WARN 已修。证据见 `build/dev011/` 与 [验证记录](docs/b2-compression-validation.md#dev-011seedarchive_integration-偶发失败排查2026-11-12)。后续：`store_archive_tests`、`codec_tests`、`store_acceptance_tests` 的夹具清理仍用 `remove_all`，在 GNU 工具链下理论上同样可能卡死，未改。

- B2-r5 下一步（P1，未做）：引入独立授权语料，预先划分开发集与评估集，在评估集上一次性检验 `adaptive-file-r1`；不在现有 5 组样本上调阈值。本轮未提交推送。

- DEV-009（B2-r4 P0）已解释 text / mixed 差距：主因是 B-r1 把约 1/3 块计划成 Store。建议候选（未实施，需用户评估）：B 的 Codec 字段不再把 Store 作为可计划项、交给 core 回退；后续 Seed 对照至少包含 B1-fixed-zstd3 与 B2-file-codec 基线并计入试编码成本与元数据。下一步按评估计划 P1 扩充代表性授权语料。本轮未提交推送。

- DEV-008已完成B2-r3真实压缩首轮。下一步评估小型基线并选择更有代表性的授权数据，再设计公平预算的候选/搜索对照；固定Codec分配中的Store占比代价需要考虑。最终Seed编码、身份和映射仍可探索。当前B2-r1/r2/r3与文档规则改动均未提交推送；用户原稿与旧实验未改写。

- 2026-11-12 用户最新方向：优先种子设计，先比较候选、让用户测试语义，再选择实验方案；不提前冻结位宽 / 字段 / PRNG；后续持续探索，通过表示与映射解耦、候选修订和旧数据兼容保留修改空间，不预建插件系统。设计输入见 [Seed 设计探索](research/ideas/seed-design-study.md)。GUI 仅在交互测试需要时启动；Actions 延后，本轮未创建工作流。M4-A与B2-r3小型真实压缩实验已完成；下一步评估基线及更有代表性的语料，再决定搜索对照，不自动冻结协议。

- CLion 旧手工 Debug profile 缺 vcpkg 工具链问题已修复（IDE-002）：当前使用 `cmake-build-debug-vcpkg`，MCP 重新加载、IDE CLI 和 codec 测试均成功；旧目录保留。用户人工补查最后一个旧测试文件仅拼写提示，忽略。历史 IDE 运行失败已由本次成功验证补齐，详见 M3 记录末节。

- M3 2026-11-12 独立验收通过：独立验收阶段只改文档，随后修复 CLion 本地配置；Release / ASan 各 30/30，IDE 13 文件完整诊断、1 文件人工补查仅拼写提示，IDE CLI / codec 运行已补齐。M3 实现及记录按用户授权提交交付，限制见 M3 记录。用户想法两份哈希未变；下一步 M4。

- M0 已核对实际工具链并完成验证；GNU presets 要求当前终端设置 SEEDARC_UCRT64_ROOT，ASan 运行时路径按实际 VC Tools 安装位置设置，详见构建说明。未验证 Windows 之外平台。
- 之前 Gopeed MCP 不可用；许可证正文已从本机标准副本取得，此问题不阻塞工程骨架。
- Git 默认 Schannel 访问曾失败，单次 OpenSSL 查询成功；沙盒读取用户全局 ignore 文件受限，项目内规则已检查。未改系统或全局 Git 配置。
- M1 已由用户验收（已知限制：真实 SUBST 盘符未测、Fuzz 仅短跑），首次提交 `fbd3fb8` 已推送到 origin/main。外部审查阶段 1 修复（REV-001）已提交为 `4c25a27`（本地 main 与 origin/main 的跟踪引用一致，未联网复核），任务仍待用户验收。
- M2（DEV-003）P1 / P2 已在代码 `4c547b2` 修复并独立复验通过；本轮仅更新验收 / 状态文档，未提交或推送。私有 DACL 行为已确认；ASan 需 VC Tools `bin\Hostx64\x64` 在进程 PATH。用户想法两份 SHA256 未变，测试无残留，无后台任务；下一步为 M4；M3 独立验收见 ACC-003，本轮不启动新阶段。
- **M2 独立验收历史**：首轮 Release 24/24，但新增回归在 Release / CLion Debug 复现两项缺陷而未通过；本轮修复后已通过上述独立复验。历史失败、修复、最新证据分别保留在 [验收记录](docs/m2-acceptance.md)，不删除负面证据。需要 IDE 前台时先提醒用户配合。
- Git 仓库属主为沙盒账户，当前用户执行 git 报 dubious ownership；本轮仅以单次 `git -c safe.directory=…` 读取状态，未修改全局 Git 配置（是否添加 safe.directory 由用户决定）。
- 2026-11-12 用户确认 CLion 改为阶段性集中检查：日常小步修改运行编译器 / 测试 / 格式检查，完整研究迭代、功能接入或里程碑验收前统一检查相关文件；IDE 独有问题、配置问题及用户指定提示按需提前检查，纯文档修改免检。所有 Agent 遵守，协调者可集成后统一检查；异常记录未覆盖范围并继续独立工作，不冒充通过。规则已同步 AGENTS.md 与开发说明。需要诊断时仍先确认前台，逐文件打开再查询。历史 REV-001 诊断证据保留于任务表。
- DEV-014（2026-11-13，Linux L1，分支 `linux-l1` 未推送）：核心库、codec 与不碰文件系统的测试已在 WSL Ubuntu 26.04 实际通过（见 [L1 记录](docs/linux-l1-validation.md)）。Linux 上输入枚举（L2）与安全输出（L3）仍返回 `unsupported_feature`；路径校验只实现 ASCII 子集，非 ASCII 路径明确拒绝。合并前待办：协调者做 CLion 诊断，补跑 MSVC ASan / UCRT64；L2 / L3 按 [草案](docs/posix-filesystem-design.md) 另行立项，L3 需 ADR。

## 更新约定

开始、完成、阻塞、交接时更新日期、任务行和必要证据；负责人协调共享文件写入，不覆盖其他任务记录。详细日志引用文件，不复制到此处。修改长期目标时同步开发计划；研究结论必须链接对应 run ID。

## 用户想法已读快照

读取日期：2026-11-11；读取者：主 Agent（2026-11-12 种子路线调整前复核：文件清单与两份 SHA256 均未变化）。目录：[docs/users_idea/](docs/users_idea/)。这是一份已读基线，不代表后台监控。后续开发 / 规划、关键决定和交接时重新比较文件清单及 SHA256，变化后阅读并覆盖更新本节，不修改用户原稿。

| 文件 | 已读 SHA256 | 评估与处理 |
| --- | --- | --- |
| [历史 v0.2 构想](docs/users_idea/SeedPack_项目设计方案_v2.md) | `550DD2BA06CE33032850AD6D973E05C0A8F9FC58AEAEE8D1201F71745E61D715` | 基础方向已纳入路线；旧名 / 扩展名 / 阶段示例不作为现行契约，GUI 按用户最新决定在人工交互测试需要时启动 |
| [结构化种子草案](docs/users_idea/SeedArc_结构化种子设计草案.md) | `368AD5B9138F5AB3691DEF36CD0A190D70DDE5EB8086AA956B7D1AA7D6FB376A` | 纳入候选：M1 评估编码 / 版本边界，M4 字段语义与影响隔离测试，M5 随机搜索对照下的局部变异；缓存须测量并验证完整键 / 失效。128 位、字段位宽与搜索方法仍待 ADR / 实验，不提前冻结 |

本轮仅补充开发输入与检查规则，不实现种子 / 缓存、不运行性能实验。重大冲突按 AGENTS 权限模型处理，不能由草案自动覆盖已确认决策。
