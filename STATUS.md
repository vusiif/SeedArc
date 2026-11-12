# SeedArc 当前状态与协作交接

更新日期：2026-11-12。此文件是当前进度入口；[开发计划](docs/development-plan.md) 保存长期路线，[research](research/README.md) 保存研究证据。

## 当前基线

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
| M4 | 种子设计探索、确定性与 MVP | M4-A 原型验证通过，待用户语义评估 | 先比较候选并由用户评估语义，再选择实验方案接入；固定向量、归档复现、动态检查与基线 |
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
| DEV-005 | M4-A 种子候选原型与小型 M3 基线 | 主 Agent；子任务 m4_seed_prototype | core/seed、research/tools、相关 CMake / 测试、third_party/splitmix64；research 配置 / 脚本 / 本轮实验 / 文档 | 原型通过，待用户评估 | 主线程 Release / ASan 各 32/32、Clang 定向 2/2，跨工具链 4 组输出字节一致；6 文件完整 IDE 诊断无 WARN / ERROR，IDE 两个目标运行成功；基线 27/27，通过范围与限制见 [M4-A 记录](docs/m4a-validation.md) |
| PLAN-004 | Seed 探索路线与优先级调整 | 主 Agent | 开发计划、架构、research/ideas、STATUS | 完成（仅设计记录） | 位宽 / 字段 / 映射未冻结；新增表示与映射替换边界、候选修订及旧包解码约束，加入 Agent 规范；A/B/C 候选与用户评估关口，GUI 按需、Actions 延后；未实施算法或运行实验，见 [设计探索](research/ideas/seed-design-study.md) |
| IDE-002 | CLion 默认 Debug 依赖配置修复 | 主 Agent | 本地 CLion Debug profile、开发说明、M3 记录、STATUS | 完成 | MCP 重新加载成功；确认 MSVC / C++23 模式与 vcpkg 依赖；IDE CLI 版本、codec 测试均 exitCode=0，CMake 文件完整诊断无问题，见 M3 记录末节 |
| ACC-003 | M3 独立验收 | 本轮主 Agent；只读子审查 m3_review | STATUS、M3 验收记录；不改原实现 | 完成 | Release / ASan 各 30/30；format / diff 检查通过，IDE 13/14 文件完整无 ERROR；1 文件人工补查仅拼写提示；IDE 运行已在 IDE-002 补齐，详见 [M3 记录](docs/m3-validation.md#独立验收2026-11-12) |
| DEV-004 | M3 Zstd / LZMA2 块编码 | 主 Agent | vcpkg manifest、CMakePresets、core/codec、archive / store_archive / safe_output、CLI、tests、fuzz 语料、ADR-0003、M3 记录、README、开发说明、STATUS | 完成（独立验收通过） | zstd 1.5.7 / liblzma 5.8.4 经 vcpkg 钉住；七套配置 + ASan 30/30，format / tidy 通过，archive Fuzz 180 s 无崩溃；CLion 诊断 13 个文件无 ERROR，修 2 条 WARN 后 MSVC Release / Clang Debug / GCC Release 复测 30/30；[M3 记录](docs/m3-validation.md) |
| DEV-002 | M1 实验格式与安全 I/O 设计 | 主 Agent；两个已结束子任务 | 候选 ADR、archive / filesystem 模块、tests、验证记录 | 完成 | 用户 2026-11-11 验收。error 5（沙盒 ACL）定位、提交重命名修复；卷身份 / 深度预算 / 共享模式 / 故障注入 / Fuzz 完成；六套 + ASan 18/18，format / tidy 通过；CLion 诊断完成并修 1 个 ERROR；真实 SUBST 未测；[M1 交接](docs/m1-validation.md) |

## 验证与证据

- 文档及模板检查：本地 Markdown 链接目标存在、JSON 模板可解析、研究归档目录齐备。
- M0：MSVC 19.51、GCC 16.2、Clang 22.1 的 Debug / Release 编译、链接、运行及 CTest 全部通过，每套 13 项；MSVC Release ASan 13/13，clang-format 与 clang-tidy 的分析器检查通过。命令、范围和日志位置见 [M0 验证记录](docs/m0-validation.md)。
- M2（2026-11-11）：七套配置全量重建后 CTest 24/24、构建 0 警告、MSVC ASan 无报告；`clang-format` / `clang-tidy` 通过；64 MiB 单文件往返峰值工作集增长约 1.4 MiB（断言上界 16 MiB）；Fuzz 短跑结果、命令、日志位置和未验证项见 [M2 记录](docs/m2-validation.md)。本轮未取得 CLion 诊断（clion MCP 服务 ECONNREFUSED）。
- M4-A：两个本轮实验为 20261112-m3-baseline-main-001、20261112-seed-plans-main-001；前者为小型单块合成基线，后者仅观察拟议计划。尚未运行真实数据 / Seed 搜索性能评估、持续 Fuzz。
- 本轮子任务 m4_seed_prototype 已交还写权限；无运行中的构建 / 研究任务。实验事实与源码快照见上述 run ID。
- CLion MCP：通过客户端已有配置发现回环服务（不硬编码连接地址），initialize 确认为 CLion MCP Server 2026.3.0.1；实际调用 tools/list、get_run_configurations、get_compiler_info、open_file_in_editor、get_all_open_file_paths 和 get_file_problems。确认 SeedArc 项目、MSVC Debug、`-std:c++latest` 和 `/Zc:__cplusplus`；活动编辑器为 cli/main.cpp。当前会话无原生 JetBrains 工具入口，使用已配置服务的实际 MCP JSON-RPC 调用，不以命令行分析冒充 IDE。
- M1 第三轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 18/18（新增 fuzz 语料重放 2 项）；`msvc-fuzz` libFuzzer header 3065144 次 / path 576342 次（各 151 s）无崩溃；format / tidy（13 个 .cpp）通过；CLion 仍 timedOut。用户想法 SHA256 复核未变。
- M1 第二轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 16/16；clang-format / clang-tidy（全部 10 个 .cpp）通过；CLion get_file_problems 对 4 个文件仍 timedOut。详见 [M1 记录](docs/m1-validation.md)。用户想法两份 SHA256 复核未变。
- CLion 检查：早期超时的根因已在 IDE-001 定位（只分析可见编辑器、窗口需在前台），方法见[开发说明](docs/development.md#clion-mcp-检查)。返回 `timedOut=true` 的空结果不代表没有诊断。

## 阻塞、限制与下一步

- 2026-11-12 用户最新方向：优先种子设计，先比较候选、让用户测试语义，再选择实验方案；不提前冻结位宽 / 字段 / PRNG；后续持续探索，通过表示与映射解耦、候选修订和旧数据兼容保留修改空间，不预建插件系统。设计输入见 [Seed 设计探索](research/ideas/seed-design-study.md)。GUI 仅在交互测试需要时启动；Actions 延后，本轮未创建工作流。M4-A 第一轮规则与小型基线已完成；下一步请用户评估字段语义与修改粒度，再决定候选迭代，不提前推进归档集成。

- CLion 旧手工 Debug profile 缺 vcpkg 工具链问题已修复（IDE-002）：当前使用 `cmake-build-debug-vcpkg`，MCP 重新加载、IDE CLI 和 codec 测试均成功；旧目录保留。用户人工补查最后一个旧测试文件仅拼写提示，忽略。历史 IDE 运行失败已由本次成功验证补齐，详见 M3 记录末节。

- M3 2026-11-12 独立验收通过：独立验收阶段只改文档，随后修复 CLion 本地配置；Release / ASan 各 30/30，IDE 13 文件完整诊断、1 文件人工补查仅拼写提示，IDE CLI / codec 运行已补齐。M3 实现及记录按用户授权提交交付，限制见 M3 记录。用户想法两份哈希未变；下一步 M4。

- M0 已核对实际工具链并完成验证；GNU presets 要求当前终端设置 SEEDARC_UCRT64_ROOT，ASan 运行时路径按实际 VC Tools 安装位置设置，详见构建说明。未验证 Windows 之外平台。
- 之前 Gopeed MCP 不可用；许可证正文已从本机标准副本取得，此问题不阻塞工程骨架。
- Git 默认 Schannel 访问曾失败，单次 OpenSSL 查询成功；沙盒读取用户全局 ignore 文件受限，项目内规则已检查。未改系统或全局 Git 配置。
- M1 已由用户验收（已知限制：真实 SUBST 盘符未测、Fuzz 仅短跑），首次提交 `fbd3fb8` 已推送到 origin/main。外部审查阶段 1 修复（REV-001）已提交为 `4c25a27`（本地 main 与 origin/main 的跟踪引用一致，未联网复核），任务仍待用户验收。
- M2（DEV-003）P1 / P2 已在代码 `4c547b2` 修复并独立复验通过；本轮仅更新验收 / 状态文档，未提交或推送。私有 DACL 行为已确认；ASan 需 VC Tools `bin\Hostx64\x64` 在进程 PATH。用户想法两份 SHA256 未变，测试无残留，无后台任务；下一步为 M4；M3 独立验收见 ACC-003，本轮不启动新阶段。
- **M2 独立验收历史**：首轮 Release 24/24，但新增回归在 Release / CLion Debug 复现两项缺陷而未通过；本轮修复后已通过上述独立复验。历史失败、修复、最新证据分别保留在 [验收记录](docs/m2-acceptance.md)，不删除负面证据。需要 IDE 前台时先提醒用户配合。
- Git 仓库属主为沙盒账户，当前用户执行 git 报 dubious ownership；本轮仅以单次 `git -c safe.directory=…` 读取状态，未修改全局 Git 配置（是否添加 safe.directory 由用户决定）。
- 后续开发使用 CLion MCP 获取相关文件 warning / error：CLion 在前台，逐个 `open_file_in_editor` 后查询。REV-001 改动的 6 个 C++ 文件已复核（见任务表）。

## 更新约定

开始、完成、阻塞、交接时更新日期、任务行和必要证据；负责人协调共享文件写入，不覆盖其他任务记录。详细日志引用文件，不复制到此处。修改长期目标时同步开发计划；研究结论必须链接对应 run ID。

## 用户想法已读快照

读取日期：2026-11-11；读取者：主 Agent（2026-11-12 种子路线调整前复核：文件清单与两份 SHA256 均未变化）。目录：[docs/users_idea/](docs/users_idea/)。这是一份已读基线，不代表后台监控。后续开发 / 规划、关键决定和交接时重新比较文件清单及 SHA256，变化后阅读并覆盖更新本节，不修改用户原稿。

| 文件 | 已读 SHA256 | 评估与处理 |
| --- | --- | --- |
| [历史 v0.2 构想](docs/users_idea/SeedPack_项目设计方案_v2.md) | `550DD2BA06CE33032850AD6D973E05C0A8F9FC58AEAEE8D1201F71745E61D715` | 基础方向已纳入路线；旧名 / 扩展名 / 阶段示例不作为现行契约，GUI 按用户最新决定在人工交互测试需要时启动 |
| [结构化种子草案](docs/users_idea/SeedArc_结构化种子设计草案.md) | `368AD5B9138F5AB3691DEF36CD0A190D70DDE5EB8086AA956B7D1AA7D6FB376A` | 纳入候选：M1 评估编码 / 版本边界，M4 字段语义与影响隔离测试，M5 随机搜索对照下的局部变异；缓存须测量并验证完整键 / 失效。128 位、字段位宽与搜索方法仍待 ADR / 实验，不提前冻结 |

本轮仅补充开发输入与检查规则，不实现种子 / 缓存、不运行性能实验。重大冲突按 AGENTS 权限模型处理，不能由草案自动覆盖已确认决策。
