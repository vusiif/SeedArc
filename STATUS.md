# SeedArc 当前状态与协作交接

更新日期：2026-11-11。此文件是当前进度入口；[开发计划](docs/development-plan.md) 保存长期路线，[research](research/README.md) 保存研究证据。

## 当前基线

- M0 工程骨架已完成：C++23 核心库、CLI、CMake presets 与 CTest。
- M2 验收缺陷 P1 / P2 **已修复，待用户复验**：暂存目录私有 DACL + 发布前整树复核（清单 / 身份 / 链接 / 命名流 / 大小 / CRC32C），管线入口与发布前检查取消；七套配置（含 ASan）25/25，CLion 无 ERROR。发布树保留私有 DACL、不继承父目录 ACL（用户 2026-11-11 已确认接受）。M2 整体仍待用户复验，不进入 M3。见 [验收与修复记录](docs/m2-acceptance.md)、[ADR-0002 §8.4](docs/decisions/0002-m2-store-archive-constraints.md)、[M2 实现记录](docs/m2-validation.md)。
- M1 已完成并由用户验收：实验头部 / 块范围 / 路径校验和安全 I/O 原语，含卷身份、累计深度 / 路径预算、共享模式调整与故障注入测试；头部 / 路径 libFuzzer 短跑无崩溃。六套配置与 MSVC ASan 均 18/18、无警告；CLion 诊断已完成（修复 1 个 ERROR）。未接 CLI，见 [接续记录](docs/m1-validation.md)与[候选 ADR](docs/decisions/0001-experimental-container-and-output.md)。
- 已有项目规范、开发路线、GPL-3.0-or-later 声明及正文，Git 远端 origin 为 https://github.com/vusiif/SeedArc（main 已推送首次提交 `fbd3fb8`）。
- 研究目录和模板已建立；尚未运行压缩实验，没有基准数据或独创性验证结论。
- third_party 目录和来源记录规范已建立，未引入实际组件；轻量 Qt GUI 计划在 M4 验收后启动。

## 里程碑

| ID | 目标 | 状态 | 完成门槛 / 证据 |
| --- | --- | --- | --- |
| M0 | C++23 / CMake / CLI / CTest 骨架 | 完成 | 三套工具链 Debug / Release 各 13/13，另有 MSVC ASan 13/13；见验证记录 |
| M1 | 实验格式与安全 I/O | 完成（用户 2026-11-11 验收） | 六套配置 + ASan 18/18，故障注入与 Fuzz 短跑通过，CLion 诊断无剩余 ERROR；已知限制：真实 SUBST 未测、Fuzz 仅短跑；见 M1 记录 |
| M2 | Store 归档往返 | 已修复，待复验 | P1 / P2 修复后七套配置 + ASan 25/25；见 [验收记录](docs/m2-acceptance.md) |
| M3 | Zstd / LZMA2 | 待开始 | 每算法往返、边界测试与依赖记录 |
| M4 | 种子确定性与 MVP | 待开始 | 固定向量、归档复现、动态检查与基线 |
| UI-1 | M4 后的轻量 Qt GUI | 待开始 | 基础操作、往返与错误显示、界面响应，核心无 Qt 依赖 |
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
| DEV-003 | M2 Store 归档端到端 | 原实现会话；验收修复：主 Agent | core、cli、tests、fuzz、ADR-0002、M2 记录、README、STATUS | 待验收 | P1 / P2 已修复；七套配置 + ASan 25/25，format / tidy 通过，CLion 4 文件无 ERROR；Fuzz 未重跑；发布树 ACL 行为已获用户确认；[验收记录](docs/m2-acceptance.md) |
| DEV-002 | M1 实验格式与安全 I/O 设计 | 主 Agent；两个已结束子任务 | 候选 ADR、archive / filesystem 模块、tests、验证记录 | 完成 | 用户 2026-11-11 验收。error 5（沙盒 ACL）定位、提交重命名修复；卷身份 / 深度预算 / 共享模式 / 故障注入 / Fuzz 完成；六套 + ASan 18/18，format / tidy 通过；CLion 诊断完成并修 1 个 ERROR；真实 SUBST 未测；[M1 交接](docs/m1-validation.md) |

## 验证与证据

- 文档及模板检查：本地 Markdown 链接目标存在、JSON 模板可解析、研究归档目录齐备。
- M0：MSVC 19.51、GCC 16.2、Clang 22.1 的 Debug / Release 编译、链接、运行及 CTest 全部通过，每套 13 项；MSVC Release ASan 13/13，clang-format 与 clang-tidy 的分析器检查通过。命令、范围和日志位置见 [M0 验证记录](docs/m0-validation.md)。
- M2（2026-11-11）：七套配置全量重建后 CTest 24/24、构建 0 警告、MSVC ASan 无报告；`clang-format` / `clang-tidy` 通过；64 MiB 单文件往返峰值工作集增长约 1.4 MiB（断言上界 16 MiB）；Fuzz 短跑结果、命令、日志位置和未验证项见 [M2 记录](docs/m2-validation.md)。本轮未取得 CLion 诊断（clion MCP 服务 ECONNREFUSED）。
- 尚未运行：压缩率 / 性能实验（Store 不压缩；M2 记录中的耗时是单次功能性观测，不是基准）；持续 Fuzz。
- 暂无实验 run ID、执行者会话 ID 或后台研究任务。
- CLion MCP：通过客户端已有配置发现回环服务（不硬编码连接地址），initialize 确认为 CLion MCP Server 2026.3.0.1；实际调用 tools/list、get_run_configurations、get_compiler_info、open_file_in_editor、get_all_open_file_paths 和 get_file_problems。确认 SeedArc 项目、MSVC Debug、`-std:c++latest` 和 `/Zc:__cplusplus`；活动编辑器为 cli/main.cpp。当前会话无原生 JetBrains 工具入口，使用已配置服务的实际 MCP JSON-RPC 调用，不以命令行分析冒充 IDE。
- M1 第三轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 18/18（新增 fuzz 语料重放 2 项）；`msvc-fuzz` libFuzzer header 3065144 次 / path 576342 次（各 151 s）无崩溃；format / tidy（13 个 .cpp）通过；CLion 仍 timedOut。用户想法 SHA256 复核未变。
- M1 第二轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 16/16；clang-format / clang-tidy（全部 10 个 .cpp）通过；CLion get_file_problems 对 4 个文件仍 timedOut。详见 [M1 记录](docs/m1-validation.md)。用户想法两份 SHA256 复核未变。
- CLion 检查：早期超时的根因已在 IDE-001 定位（只分析可见编辑器、窗口需在前台），方法见[开发说明](docs/development.md#clion-mcp-检查)。返回 `timedOut=true` 的空结果不代表没有诊断。

## 阻塞、限制与下一步

- M0 已核对实际工具链并完成验证；GNU presets 要求当前终端设置 SEEDARC_UCRT64_ROOT，ASan 运行时路径按实际 VC Tools 安装位置设置，详见构建说明。未验证 Windows 之外平台。
- 之前 Gopeed MCP 不可用；许可证正文已从本机标准副本取得，此问题不阻塞工程骨架。
- Git 默认 Schannel 访问曾失败，单次 OpenSSL 查询成功；沙盒读取用户全局 ignore 文件受限，项目内规则已检查。未改系统或全局 Git 配置。
- M1 已由用户验收（已知限制：真实 SUBST 盘符未测、Fuzz 仅短跑），首次提交 `fbd3fb8` 已推送到 origin/main。外部审查阶段 1 修复（REV-001）已提交为 `4c25a27`（本地 main 与 origin/main 的跟踪引用一致，未联网复核），任务仍待用户验收。
- M2（DEV-003）P1 / P2 **已修复，工作区未提交**，等待用户复验；“发布树保留私有 DACL、不继承父目录 ACL”已于 2026-11-11 获用户确认。ASan 运行需将 VC Tools `bin\Hostx64\x64` 加入 PATH。用户想法两份 SHA256 复核未变。无仍在运行的子任务、测试或 Fuzz。
- **M2 独立验收补充**：用户打开 CLion 并置前台后，MCP 已检查 store_archive.cpp、archive_index.cpp、input_tree.cpp、safe_output.cpp 及新增回归测试，完整返回无 ERROR，保留有依据的 WARN / INFO；明细与判断见 [验收记录](docs/m2-acceptance.md)。以后需前台时先提醒用户配合。原有 Release 24/24 独立通过；新增回归在 Release / CLion Debug 均复现两个缺陷，因此仍未通过验收。测试临时数据已清理。
- Git 仓库属主为沙盒账户，当前用户执行 git 报 dubious ownership；本轮仅以单次 `git -c safe.directory=…` 读取状态，未修改全局 Git 配置（是否添加 safe.directory 由用户决定）。
- 后续开发使用 CLion MCP 获取相关文件 warning / error：CLion 在前台，逐个 `open_file_in_editor` 后查询。REV-001 改动的 6 个 C++ 文件已复核（见任务表）。

## 更新约定

开始、完成、阻塞、交接时更新日期、任务行和必要证据；负责人协调共享文件写入，不覆盖其他任务记录。详细日志引用文件，不复制到此处。修改长期目标时同步开发计划；研究结论必须链接对应 run ID。

## 用户想法已读快照

读取日期：2026-11-11；读取者：主 Agent（M2 开工前复核：文件清单与两份 SHA256 均未变化）。目录：[docs/users_idea/](docs/users_idea/)。这是一份已读基线，不代表后台监控。后续开发 / 规划、关键决定和交接时重新比较文件清单及 SHA256，变化后阅读并覆盖更新本节，不修改用户原稿。

| 文件 | 已读 SHA256 | 评估与处理 |
| --- | --- | --- |
| [历史 v0.2 构想](docs/users_idea/SeedPack_项目设计方案_v2.md) | `550DD2BA06CE33032850AD6D973E05C0A8F9FC58AEAEE8D1201F71745E61D715` | 基础方向已纳入路线；旧名 / 扩展名 / 阶段示例不作为现行契约，GUI 按已确认的 M4 后安排 |
| [结构化种子草案](docs/users_idea/SeedArc_结构化种子设计草案.md) | `368AD5B9138F5AB3691DEF36CD0A190D70DDE5EB8086AA956B7D1AA7D6FB376A` | 纳入候选：M1 评估编码 / 版本边界，M4 字段语义与影响隔离测试，M5 随机搜索对照下的局部变异；缓存须测量并验证完整键 / 失效。128 位、字段位宽与搜索方法仍待 ADR / 实验，不提前冻结 |

本轮仅补充开发输入与检查规则，不实现种子 / 缓存、不运行性能实验。重大冲突按 AGENTS 权限模型处理，不能由草案自动覆盖已确认决策。
