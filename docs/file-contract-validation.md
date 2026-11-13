# B2-r2 文件身份与叠加行为验证

日期：2026-11-12（America/Los_Angeles）。任务 DEV-007。主 Agent 实现与验证。结论：**计划层契约验证通过，M4 仍在探索阶段**。

## 实现与决定

在独立研究库增加 core/seed/path_overrides.hpp / .cpp。catalog-r1 通过完整文件清单把精确规范路径绑定到本次 file_id，再复用 overlay-r1；包含显式空文件。路径验证复用现有 Windows UTF-8 / NFC、分隔符、大小写冲突与祖先冲突规则，没有新的归一化或路径解析器。研究库链接已有 SeedArc::core 以调用校验器，生产 core 不依赖研究层，无新增软件包。

清单或覆盖数量先限额，路径字节预算先检查再复制；未知文件 / 块归属、重复 ID / 路径 / 覆盖、非法等级、空覆盖、未知修订明确拒绝。匹配精确区分大小写；清单的大小写冲突依然拒绝。目标删除或重命名时旧键整次失败，调用方显式移除 / 重新绑定，避免静默迁移。

文件身份匹配不承诺 Codec 分配稳定：原 B-r1 仍以 global_block_id 决定 Codec。保留 ID 的清单扰动不改变原计划，重新编号允许改变 Codec；文件覆盖仍绑定同路径。本轮只编辑等级，未做 Codec 编辑。

新增研究工具 seedarc_seed_scope_contract（默认 OFF 的研究工具开关）与 seed.path_rank_overrides 测试。历史 A/B-r1、overlay-r1 源码与实验未改写。规则详见 [B2-r2](../research/ideas/seed-file-contract-r2.md)，原始证据与 [可读对照](../research/experiments/20261112-file-contract-main-001/preview.md) 一并归档。

## 实际检查

| 检查 | 结果 | 日志（项目 build/） |
| --- | --- | --- |
| MSVC Release 全量构建 / CTest | 34/34，51.70 s，无编译警告 | file-contract-release-build-normal.log、file-contract-release-test.log |
| 两条 IDE 风格提示修正后 Release 定向回归 | 4/4，0.12 s，新旧种子与 M3 兼容 | file-contract-release-final-{build,test}.log |
| ASan Release 定向 | 4/4，1.21 s，无报告；最终测试源码修正后新测试1/1，0.53 s | file-contract-asan-{build,test}.log、file-contract-asan-final-{build,test}.log |
| Clang UCRT64 Debug 定向 | 3/3，0.28 s，无编译警告 | file-contract-clang-build-normal.log、file-contract-clang-test.log |
| MSVC Release / Clang Debug 工具输出 | 86434字节原始 stdout 完全一致 | 实验 config.json / observation.json |
| 工具 CLI 边界 | 无参数 / 额外参数 / 未知参数退出2且 stdout 为空，help退出0，共4项通过 | 实验 cli-smoke.json |
| CLion | 4个新增 C++ 文件与3个 CMake 文件完整 warnings 诊断，无 WARN / ERROR；新测试与工具 IDE 运行退出0 | file-contract-ide-*.json |
| 格式与研究文件 | 新增4个 C++文件 clang-format --dry-run --Werror；分析脚本语法、原始数据验证、快照哈希与本地链接核对 | 主 Agent 实际执行 |

测试覆盖完整 `3 → 4 → 6 → 4`、逐 Codec 隔离、插入 / 删除其他文件 / 重排 / ID 重建、目标删除 / 重命名失效、空文件、单块无目标 Codec、10000块单文件、大小写 / NFC / 路径冲突、错误及限额。既有 overlay-r1 测试继续覆盖逐字段继承与清除、最大块数等边界。

首轮 MSVC 与 Clang 沙盒构建均在 vcpkg applocal 调用 WindowsApps pwsh 时失败（程序执行受限）；C++ 编译成功不等于构建通过。正常用户身份重建通过，未关闭构建步骤或测试。初轮日志保留。文件系统全量测试同样使用正常用户身份，沿用已核实的 ACL 环境边界。

CLion MCP 重新加载 CMake 为 SUCCEEDED，真实配置为 MSVC Debug、C++23模式（-std:c++latest、/Zc:__cplusplus），使用现有 vcpkg profile。用户确认前台后，逐文件打开并核对活动路径，再取 errorsOnly=false 诊断。修正未知块夹具可设 constexpr、工具标签变量作用域两条 WARN 后复查无 WARN / ERROR；INFO 中指定初始化 / 结构化绑定等建议按现有命名风格保留，未禁用检查。

## 限制与下一步

没有读取真实文件、生成归档、压缩或搜索；未测量生成耗时 / 峰值内存，不从 Debug 结果得出性能结论。未新跑持续 Fuzz、完整 ASan矩阵或非 Windows 平台。既有归档语料重放包含在 Release 全量中。

空文件覆盖是可接受但无变化的状态，不表示有压缩工作。128块夹具中41个 Zstd 块被覆盖，单测另验证10000块文件；这不足以证明文件级控制适合真实异构大文件。文件级覆盖的 Seed 编码、元数据成本、最终身份策略仍未冻结。下一步可以选择小型真实数据接入具体计划做压缩对照，但应先明确实验输入和公平基线，不能以本轮观察宣称收益。本轮未提交推送。
