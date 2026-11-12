# M4-A 第一轮：种子计划原型与固定算法小型基线

日期：2026-11-12。关联 DEV-005。结论：**原型集成验证通过，待用户评估种子字段语义；M4 整体未完成**。

## 范围与设计

A-r1 为非结构化随机映射对照；B-r1 分离算法调度与 Zstd / LZMA2 等级字段，等级直接映射而不整体混合。B 暂时为同 codec 所有块使用统一等级，A 支持逐块随机等级，能力与分布不同；不能据此宣称公平搜索优劣。规则、候选身份、边界、模偏差与固定向量见 [候选 r1](../research/ideas/seed-candidates-r1.md)。

内部 `core/seed` 独立生成具体计划；研究静态目标不链接进生产 libseedarc，主 CLI、归档布局和解码路径均未改变。工具 `seedarc_seed_plan` 由默认 OFF 的 `SEEDARC_BUILD_RESEARCH_TOOLS` 启用。CLI 编辑格式只是实验字段，不是 128 位归档种子协议。未知候选 / 修订、类型不匹配与越界字段明确拒绝；按块全局序号随机访问，不共享可变 PRNG 状态。新增候选仍需建立修订及向量，不默改旧语义。

SplitMix64 单步公式按来源规范保存于 third_party/splitmix64，含作者许可、原片段、本地适配 patch 与哈希；项目封装位于 core/seed。原作者参考见该目录 UPSTREAM.md；无新增包或动态库依赖。候选规则不声称密码学安全、统计质量或独创性。

## 实验归档与人工测试入口

- [M3 合成基线](../research/experiments/20261112-m3-baseline-main-001/record.md)：已有 M3 Release 二进制，3 类输入 × 3 编码 × 3 次，27/27 文件清单与内容哈希往返通过；逐次命令、原始指标、日志和脚本快照保留。只有单块小型合成数据，内存未测量，不代表真实文件性能。
- [种子计划观察](../research/experiments/20261112-seed-plans-main-001/record.md)：两组固定编辑的原始 JSON、源码快照与哈希、执行命令及 [可读对照表](../research/experiments/20261112-seed-plans-main-001/preview.md)。A 0→1 变化 10/12 块；B Zstd 等级 3→4 仅改变 5 个 Zstd 块，其他计划不变。这是计划观察，不是压缩或搜索收益。
- M3 旧归档样例在 tests/fixtures/m3：Store 空树、Zstd / LZMA2 文本压缩块；固定哈希与来源记录保留。新增永久测试验证受限解码 / 摘要并拒绝截断；没有自动重生成覆盖旧样例。

构建和体验（项目根目录；先按开发说明设置 VCPKG_ROOT）：

```powershell
cmake --preset msvc-vs2026-release -DSEEDARC_BUILD_RESEARCH_TOOLS=ON
cmake --build --preset msvc-vs2026-release --parallel
.\build\msvc-vs2026-release\research\tools\Release\seedarc_seed_plan.exe a-r1 0 12 1
.\build\msvc-vs2026-release\research\tools\Release\seedarc_seed_plan.exe b-r1 0 2 6 12 0 3 6
```

工具只向 stdout 输出 JSON，不压缩 / 修改文件。B 命令依次为 codec 字段、Zstd rank、LZMA2 rank、块数、可选的三组对照字段；字段用十六进制，rank=2 对应 Zstd-3，块数用十进制。详见 `--help`；建议先阅读对照表，再编辑小范围输入。

## 主线程独立验证

子任务 m4_seed_prototype 实现候选、规则与工具并核对格式，未运行构建 / 测试 / IDE。以下由主线程独立执行：

| 检查 | 实际结果 | 日志 |
| --- | --- | --- |
| MSVC Release 配置开启研究工具、全量构建与 CTest | 32/32，50.86 s，无构建警告 | build/m4a-release-{configure,build,test}.log |
| 已有 MSVC ASan Release 重建与全量 CTest | 32/32，55.28 s，无构建警告、无 ASan 报告 | build/m4a-asan-{build,test}.log |
| Clang UCRT64 Debug 配置、构建研究 / 新测试目标并定向 CTest | 2/2，0.12 s，无构建警告 | build/m4a-clang-{configure,build,test}.log |
| 跨工具链计划输出比较 | 4 组各 64 块、共 256 行，MSVC Release 与 Clang Debug stdout 字节一致 | build/m4a-cross-toolchain.json |
| 研究 CLI 参数边界 | 11 组通过；非法输入退出 2、无 stdout | build/m4a-tool-smoke.json |
| clang-format 与 diff whitespace | 6 个新增 C++ 文件格式通过，git diff --check 通过 | 实际命令结果；无单独格式日志 |

32 项 = M3 的 30 项 + seed.candidate_plans + archive.m3_compatibility。候选测试包括固定向量、未知版本、字段边界、全合法等级隔离、最大 block ID 和前后遍历一致。ASan 的运行时 PATH 加入本机 VC Tools bin/Hostx64/x64，文件系统测试以正常用户身份运行，未弱化 ACL / 路径保护。测试结束 `.sandbox-test` 无残留。

## CLion MCP

用户确认前台后，旧客户端地址返回拒绝连接；用户提供当前端口，通过已有传输路径连接成功（未修改全局客户端配置）。CLion 本地 Debug profile 保留 vcpkg / Qt 选项，仅追加研究工具开关；`load_cmake` SUCCEEDED。编译器信息确认 MSVC / C++23 模式（-std:c++latest、/Zc:__cplusplus）、/MDd、实际源码与 third_party include 路径。

以下 6 文件逐个打开，以 errorsOnly=false 完整返回，无 WARN / ERROR：core/seed/strategy.cpp / .hpp、tests/seed_strategy_tests.cpp、tests/m3_compatibility_tests.cpp、research/tools/seed_plan.cpp、third_party/splitmix64/splitmix64.hpp。其余 INFO 风格提示未作无关改动，拼写提示按用户要求忽略。

现有 IDE seedarc_seed_strategy_tests 与 seedarc_seed_plan（B 编辑示例）均 exitCode=0。IDE 控制台预览可能自动折行，JSON 原始字节证据使用独立 CLI stdout。日志 build/m4a-ide-reload.json、m4a-ide.json、m4a-ide-run.json。

## 限制与下一步

用户尚未评估字段作用粒度；没有选定最终种子布局、PRNG 或搜索方案。没有 Seed 归档往返、Seed 压缩率结果或性能改进结论。未运行新 Fuzz、UBSan、GCC 或完整 Clang 矩阵；既有 Fuzz 语料重放包含于全量 CTest。ASan 不覆盖未插桩的第三方 codec 内部。

下一步请用户比较字段语义与全局等级的粒度，再决定修改 B、引入作用范围候选 C 或继续比较块身份。完成该评估后再推进小规模搜索实验与实验 ADR；不提前冻结协议。本轮未添加 GUI / Actions。用户已授权提交本轮原型与实验归档；种子字段语义评估仍待完成。
