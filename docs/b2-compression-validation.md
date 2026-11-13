# B2-r3 真实压缩接入与首轮基线

日期：2026-11-12（America/Los_Angeles）。任务 DEV-008。主 Agent 实现与验证。结论：**具体计划压缩接入、功能回归与小型可复现基线通过；M4仍未最终选型，Seed协议未冻结**。

## 实现

core/archive_plan.hpp 提供内部具体计划入口：压缩器在同一次安全枚举后，将只读规范清单交给受信进程内 PlanBuilder；返回每块Codec/config，核对总数与合法性后才读内容。普通 compress_archive 继续固定参数。生产core不依赖研究模块、不解释Seed，不新增插件、依赖或生产CLI选项。

core/seed/archive_study 复用 B-r1 / catalog-r1 / overlay-r1，把真实文件（含空文件）与块身份生成具体计划，再走原压缩管线。编码器切换时释放旧实例，只保留一个活编码器。无收益仍Store回退；每块压缩后自校验，沿用安全读取、输入变化检测、事务写出和失败清理。

原归档头、块表与解码逻辑未变；仍strategy_version=0、seed_length=0，逐块保存实际Codec/config，解码不运行Seed映射或搜索。研究配置与拟议计划外置实验记录，本轮未评价最终Seed元数据成本。规则见 [r3](../research/ideas/seed-compression-r3.md)。

`block_index` 是当前数组位置，`block_id` 是逻辑global_block_id，`file_block_index` 是文件内编号，路径是覆盖键。r2的105合法、不是9行数组下标；本次真实压缩中block_id恰好取本次规范块序号0..N-1，仍不承诺插入后的Codec稳定。

## 实际验证

| 检查 | 实际结果 | 证据（build/；实验内另存快照） |
| --- | --- | --- |
| MSVC Release 全量 | 35/35，52.38s | b2-r3-release-{build,test}.log |
| MSVC ASan Release 全量 | 35/35，62.12s，无ASan报告 | b2-r3-asan-{build,test}.log |
| Clang UCRT64 Debug 定向 | 新旧seed / 旧包兼容5/5，0.62s | b2-r3-clang-{build,test}.log |
| IDE修正后定向回归 | Release8/8（6.37s），ASan新集成1/1（0.26s），Clang新集成1/1（0.24s） | b2-r3-final-{release,asan,clang}-{build,test}.log |
| 真实磁盘基线 | 5类×8方案×3次=120/120，compress→verify→extract逐字节还原 | [实验](../research/experiments/20261112-b2-compression-main-001/record.md) raw.json / summary.json |
| 双向兼容 | 已验收M3旧CLI验证/提取本轮8种混合目录归档；当前程序验证/提取3个旧样例，内容准确 | 实验 compatibility.json |
| 跨工具链观察 | 5类b-file的计划、实际块描述与归档字节在本机MSVC Release / Clang Debug一致 | 实验 cross-toolchain.json |
| CLI边界与Unicode | 7项错误/帮助/冲突检查通过，另有非ASCII归档路径与目标键成功 | 实验 cli-smoke.json |
| 格式与归档检查 | 6个相关C++文件clang-format --dry-run --Werror；源码快照/哈希、JSON、文档链接、差异检查 | 主 Agent 实际执行 |

新 CTest seed.archive_integration 验证四状态实际编码、独立验证与提取、逐字节还原、清除归档与当前全局归档相同、实际非Store codec/config与计划一致、极短块Store回退、空文件与无目标Codec、固定计划与原公共入口逐字节一致、已有输出不覆盖。负测包含未知候选修订、非法等级、未知路径、计划数量不足/错误Codec/config/构建失败、空builder、调用前与规划时取消、规划后输入变化、截断归档和损坏块；失败不得发布且无临时残留。

初次集成测试失败于测试辅助函数将整包传给严格96字节头部解码器；改为精确头部切片后通过。首轮研究工具构建失败于格式化后psapi.h排在windows.h前；沿用项目已有分组方式修正。失败日志保留，不计为通过。失败测试夹具在保存日志、核对本任务绝对路径和无重解析点后清理。

## CLion阶段检查

用户确认前台后集中检查6个C++文件与3个CMake文件，errorsOnly=false，最终均无ERROR。修正工具u8path弃用（改为C++20 char8_t路径构造）、局部常量/作用域/内部链接建议，减少新增内部stop_token转发复制。core/store_archive.cpp保留2条公开入口stop_token按值传递建议：沿用现有公共签名与值语义，未为清零改动公共API；其余新增文件与CMake无WARN。拼写提示忽略，INFO风格建议未压制。

真实配置为MSVC Debug、C++23模式（-std:c++latest、/Zc:__cplusplus）、既有vcpkg profile，MCP CMake重载SUCCEEDED。IDE新测试与工具verify运行均退出0。中途一次诊断未完整返回，不计入覆盖，随后补齐。日志为b2-r3-ide-*.json；按新规范本阶段集中检查，日常小修改未逐轮要求IDE。

## 基线结果与限制

[汇总表](../research/experiments/20261112-b2-compression-main-001/preview.md)记录完整归档体积、压缩/验证/解压进程墙钟时间与峰值工作集。5类输入为项目源码原件及自制文本、二进制、gzip和混合目录；块大小64KiB，单线程，每组3次，轮转顺序，无搜索。混合目录含空文件及Unicode路径。

严格比较相同Codec分配的b-global与b-file：源码样本小667字节，其余4类体积相同。固定单算法参考使用不同Codec分配，部分样本优于B，不掩盖负面结果。不能据此声称B优于A、普遍提高压缩率或搜索更高效。

时间包含进程启动、计划生成、自校验及JSON证据输出；内存为GetProcessMemoryInfo的PeakWorkingSetSize，包含工具/报告，不是纯codec内存。未控制OS缓存，无统计显著性分析。样本小、非独立代表性语料、没有调参/搜索评估或最终Seed编码。未新跑持续Fuzz、UBSan或非Windows平台；Release/ASan全量含现有Fuzz语料重放，不等于持续Fuzz。跨工具链一致仅为本轮5例观察，不承诺所有环境归档相同。

下一步先选择更有代表性的授权数据，评估固定Codec分配中Store占比的代价，再设计公平预算下的候选/搜索比较；不因本轮功能验收冻结文件身份或种子映射。本轮未提交推送，Actions与GUI继续延后。

## DEV-011：`seed.archive_integration` 偶发失败排查（2026-11-12）

**结论**：这是测试的观测问题，不是实现问题，失败写入没有留下任何残留。排查中另发现 libstdc++ 16.2 的 `remove_all` 在同一现象下会死循环（只影响测试夹具清理）。证据保存在 `build/dev011/`（`probe-results.md`、探针脚本、诊断版测试、日志）。

**现象**：断言“failed planned writes leave no temporary artifacts”在失败写入后立刻数 `work` 目录项，偶尔比基线多或少一项。

**定位**：给测试加临时诊断，在同一次遍历中同时计数和记下名字，并对差异项查询属性、打开结果、Restart Manager 持有者和消失时间。

- Release 300 次中失败 22 次，22 次全部得到解释：差异项都是 `FIXED-PUBLIC.SARC.tmp` 或 `FIXED-PLANNED.SARC.tmp`，其中 18 次落在基线一侧、4 次落在最终一侧。从未出现 SeedArc 的临时名 `.seedarc-*.tmp`，也从未出现 `bad.sarc`。
- 这些名字来自测试在记基线之前用 `fs::remove` 删除的、**已成功提交**的对照归档 `fixed-*.sarc`。

**来源（独立探针，与测试代码无关）**：

- 删除一个由 SeedArc 写出并已关闭的归档后，约 0.7 ms 会出现一个“原名转大写 + `.tmp`”的条目，约 0.2 ms 后消失。用 `DeleteFileW` 和 POSIX 语义删除都会出现，300–600 次里出现 3–4 次。
- 该条目打开即 `ERROR_FILE_NOT_FOUND`，与被删文件的 FileId 不同，Restart Manager 查不到持有者。
- Python 写出的普通文件，以及由 Python 写入 SeedArc 临时名再改名的文件，都从未出现这种条目。
- 结论：这是本进程之外的组件在删除后做出的反应。环境情况：Defender 未运行；进程里注入了 Windhawk 1.7.3，但只装了任务栏时钟 mod；`fltmc` 没有权限列出过滤驱动。**具体是哪个组件尚未查明。**

**附带发现：Clang UCRT64 Debug 下卡死**

- CTest 重复运行时，第 148 次左右超时 120 s；`hang_hunt` 第 37 次复现，进程单核 100 % 空转。
- gdb 栈：`remove_all → recursive_directory_iterator::__erase → remove → symlink_status`。在 CreateFileW 下断点，读到它反复访问的路径是 `failed-writes\.SEEDARC-<HEX>.TMP.tmp`。这是同一种外部条目，只是这次对应失败写入自己那个已被删除的临时文件。
- 卡住期间，从另一进程可以直接 `rmdir` 这个空目录，说明**没有泄漏的不共享删除句柄**。目录删掉后进程仍在空转，可见是 libstdc++ 在条目消失后不前进造成的死循环。

**判定为测试问题的依据**：

- 失败路径在返回前就通过句柄删除了临时文件；临时文件以独占方式打开，关闭最后一个句柄时名字随即移除。
- 诊断中观察到的差异项全部是外部组件列出、无法打开的名字，失败写入的真实对象从未残留。
- 生产代码不使用 `remove_all`，受影响的只是测试夹具的清理。

**改动（只改测试）**：

- `tests/seed_archive_tests.cpp`：
  - 失败写入改到专用空目录 `failed-writes/`，不再数整个 `work` 目录。
  - 对该目录里每个条目核对对象：能打开、或处于删除挂起状态（打开返回 `ERROR_ACCESS_DENIED`）的，算作残留；只有已消失（`NOT_FOUND`）的名字被豁免，并会打印出来。
  - 新增自检，证明删除挂起的对象不会被豁免，真正删除之后才被认作已消失。
  - 清理前的重解析点安全扫描，对已消失的名字视为安全。之前遇到这种名字会保守地保留夹具，而退出码仍为 0。
- `tests/seed_adaptive_tests.cpp`：采用同样的安全扫描与清理方式。
- 新增 `tests/seed_test_fixture.hpp`，提供 `vanished()` 与有界的 `remove_tree()`，用来替代 `remove_all`。没有使用 sleep。

**验证**：见 STATUS 中的 DEV-011。另外 `store_archive_tests`、`codec_tests`、`store_acceptance_tests` 的夹具清理仍在用 `remove_all`，在 GNU 工具链下理论上可能同样卡死，本轮未改，列为后续项。
