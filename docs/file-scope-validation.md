# 文件级等级覆盖原型验证

日期：2026-11-12。任务 DEV-006。结论：**实现与集成验证通过，待用户评估文件级控制粒度；M4 未完成、种子设计未冻结**。

## 实现范围

新建 core/seed/file_overrides.hpp / .cpp，提供独立 overlay-r1。输入为固定块清单中的 file_id / file_block_index / global_block_id 与按文件的可选 rank 覆盖；输出按原全局 ID 排序，不重新编号。基础计划仍调用 B-r1，A/B 原实现与向量无改动。只覆盖匹配文件和 codec 的等级，其他字段继承；移除字段 / 条目恢复当前全局默认。

先限额再复制输入：至多 65536 块、4096 个文件及 4096 条覆盖；拒绝未知修订、未知文件、重复全局 ID / 文件内 ID、重复文件覆盖、空覆盖、非法 rank。允许稀疏或最大 uint64 ID，不用 ID 直接索引数组。采用有界 vector / set / map，排序与查找不扫描无界乘积。空文件未进入块清单，不提供其编辑行为；不承诺插入 / 重命名后的文件身份稳定性。

研究工具 seedarc_seed_scope_plan 复用既有默认 OFF 的研究开关，支持 `overlay-r1 FILE_ID ZSTD_RANK|- LZMA2_RANK|-`，参数为十进制。当前工具只展示固定三文件 / 十二块，不读取用户文件、压缩、修改归档或实现新种子编码。详细规则见 [overlay-r1](../research/ideas/seed-file-overrides-r1.md)。

## 实际检查

子任务 m4_seed_prototype 实现并做格式核对，未运行构建 / 测试 / IDE；以下由主线程独立执行。

| 项目 | 实际结果 | 日志 |
| --- | --- | --- |
| MSVC Release 增量构建及全量 CTest | 33/33，49.68 s，无构建警告 | build/file-scope-release-{build,test}.log |
| IDE 提示修正后 Release 重建工具 / 新测试并定向 CTest | 原候选、新覆盖、旧归档兼容 3/3，0.23 s | build/file-scope-release-final-{build,test}.log |
| MSVC ASan Release 新旧候选与旧包兼容 | 3/3，0.80 s，无 ASan 报告、无构建警告 | build/file-scope-asan-{build,test}.log |
| Clang UCRT64 Debug 构建工具 / 新旧候选并定向 CTest | 2/2，0.17 s，无构建警告 | build/file-scope-clang-{build,test}.log |
| 跨工具链输出 | MSVC Release / Clang Debug 四组各12块 stdout 字节一致 | build/file-scope-cross-toolchain.json |
| 原 A/B 示例 | 两份输出 JSON 与前轮归档一致 | 主线程实际比较；前轮记录未改 |
| 研究工具 CLI 边界 | 12 组通过，错误输入退出2且 stdout 为空 | build/file-scope-tool-smoke.json |
| 格式 / 本地文档检查 | 4 个新增 C++ 文件 clang-format --dry-run --Werror；本地链接、JSON、快照哈希核对通过 | 实际命令结果 |

33 项为上轮32项加 seed.file_rank_overrides。新测试覆盖文件 / codec 隔离、逐字段继承、全局默认变化后的覆盖优先级、移除单字段与条目、清除恢复当前默认、顺序无关、稀疏最大 ID、数量上限及错误拒绝。运行文件系统全测采用正常用户身份，避免已确认沙盒 ACL 干扰；未弱化校验。ASan 使用既有 VC Tools 运行时 PATH，本轮未重新执行完整 ASan / Clang 矩阵。

## CLion MCP

服务连接与重新加载成功；实际编译器为 MSVC Debug、C++23 模式（-std:c++latest、/Zc:__cplusplus），vcpkg 配置沿用本机 profile。

初轮 IDE 将新头文件作为独立上下文分析，无法通过 `seed/strategy.hpp` 解析同模块头文件，虽真实编译成功，产生连带符号错误。改为同目录 `strategy.hpp` 后错误消失。新增测试中8组纯常量数组改为 constexpr，行为不变，修正后做了上述定向重测。

最终对 file_overrides.hpp / .cpp、seed_file_override_tests.cpp、seed_scope_plan.cpp 逐个打开并以 errorsOnly=false 取得完整诊断，无 ERROR。头文件保留一条“可能未使用 cstddef”的 WARN：该头直接声明 std::size_t 限额，保留显式包含；其余无 WARN。工具文件中途查询曾取消，用户人工查看无报错，随后 MCP 补查完整返回，无 WARN / ERROR。未以取消或空超时当作通过。

IDE 现有 seedarc_seed_file_override_tests、seedarc_seed_scope_plan（overlay-r1 1 3 -）均 exitCode=0。证据：build/file-scope-ide-reload.json、file-scope-ide.json（初轮）、file-scope-ide-final.json、file-scope-ide-run.json。

## 观察与下一步

[本轮实验](../research/experiments/20261112-file-scope-main-001/record.md)保存预设方法、输入、源码快照、哈希、原始 JSON、配置和分析脚本；[可读对照表](../research/experiments/20261112-file-scope-main-001/preview.md)显示全局调整影响5块、文件1覆盖仅影响2块、清除恢复基础计划。计划观察不证明压缩率或搜索收益，尚无实际种子归档或最终格式选择。

本轮无新归档解析 / 解压路径，未运行新 Fuzz、UBSan 或性能实验；既有语料重放包含于 Release 全量测试。用户语义评估仍待完成。下一步由用户判断文件级粒度是否合用，再决定作用域或种子映射迭代。没有 GUI / Actions，未提交推送。
