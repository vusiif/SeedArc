# M2 独立验收

日期：2026-11-11。结论：**未通过，需修复后复验**。本轮验收只新增定向回归测试，不修改 M2 实现，不提交 / 推送。

## 范围与证据

用户想法目录文件清单及两份 SHA256 与 STATUS 快照一致。读取 M2 设计、实现与测试记录；主线程实际执行，子任务 `/root/m2_review` 只读检查输入和事务实现，无代码修改。

- MSVC Release 构建通过，日志 `build/m2-acceptance-build.log`。
- 沙盒内原有全套测试 20/24：四个文件系统用例因 Windows error 5 / NTSTATUS c0000022 失败，往返测试进一步触发 fail-fast；不是正常用户下的验收结论。原日志 `build/m2-acceptance-ctest.log`。
- 正常用户权限下原有测试 **24/24 通过**，10.75 秒，日志 `build/m2-acceptance-user-ctest.log`。这独立确认了原测试覆盖下的往返、安全负测、故障注入、内存检查与语料重放，不代替未覆盖边界。
- 新增 `tests/store_acceptance_tests.cpp` 与 CTest `archive.store_acceptance`。Release 构建通过，定向测试失败；日志 `build/m2-acceptance-regression.log`。首次启动被聊天中断，确认无进程 / 日志后才继续，没有重复启动未知任务。
- 通过 CLion MCP 发现并运行现有 `seedarc_store_acceptance_tests` Debug 配置，临时参数为本项目 `.sandbox-test`，同样退出 1，得到相同复现结果；没有持久修改 IDE 配置。

## 必须修复

### P1：暂存树被修改后仍成功发布

位置：`core/store_archive.cpp:273` 的 `ExtractSink::end_file` 在单文件提交后立即释放独占句柄；`commit` 关闭子目录后直接改名，没有阻止已校验内容被修改或清单新增。

定向测试创建 `a=original` 和较大的 `b`。使用项目已有 `WriteFaults::after_write` 钩子，在写 b 时改写暂存目录中已经提交的 a 为 tampered，并添加 foreign.txt；不注入任何 I/O 失败。实际输出：

```text
injection_executed=1 extract_success=1 tampered_content_published=1 foreign_entry_published=1
```

正常用户 Release 和 CLion Debug 均复现。条件是其他执行者能并发写入暂存目录；当前暂存目录未建立隔离该写入的完整保护。归档内容已经验证，不等于最终发布树仍与其一致，这是“成功必须逐字节恢复”的缺口。

修复需明确并实现暂存树在校验至发布期间的保护和验证策略，包含文件内容 / 身份 / 目录清单及可被并发改写的窗口。单纯在最后再次扫描不能自动消除扫描后的竞态；不要仅删除此回归用例或放宽比较。

### P2：空归档忽略预先取消

位置：`core/store_archive.cpp:173` 仅在条目循环中检查取消；空归档跳过循环，`extract_archive` 继续在第 420 行提交目录。

对空目录生成归档，先 `request_stop()`，再调用 extract。实际输出：

```text
pre_cancelled_empty_extract_success=1 cancelled_target_exists=1
```

应返回 cancelled 且不发布目标。需在统一管线入口和发布前检查取消，补充空归档及空文件边界；同步审查 verify 的空归档取消语义。

## CLion 诊断

服务已恢复，确认 SeedArc、MSVC Debug、C++23 选项，逐个打开再查询 `errorsOnly=false`。

- archive_index.cpp：完整返回，无 ERROR / WARN，仅 INFO 风格建议。
- input_tree.cpp：完整返回，无 ERROR；WARN 为 include / 作用域 / const 建议。`<cstring>` 实际用于 std::memcpy，不为清零而删除；其余风格建议不作为本轮安全问题替代品。
- 初次 store_archive.cpp 与 safe_output.cpp 超时；按用户提醒，先协调 CLion 前台，再查询已完整返回，无 ERROR。store_archive.cpp 主要 WARN 为 stop_token 值传递建议；safe_output.cpp 有 include、作用域、stop_token 建议，以及 NT 结构 / placement new 的分析器提示。已读源码：cstring 实际用于 memcpy；UNICODE_STRING 零初始化后填写全部必需字段，OBJECT_ATTRIBUTES 由初始化宏填写后传入 NT API；placement new 使用 vector 拥有的存储且对象可平凡析构，不产生独立堆分配。保留检查，不因这些提示改动实现。
- 上述四个核心文件及新增回归测试的完整诊断保存在 `build/m2-acceptance-ide.json`，均非超时且无 ERROR。新增回归中的两处 bool 函数式转换提示已改为 has_value，不改变测试条件与输出含义。
- IDE 实际运行定向回归，Debug 同样复现两个缺陷。打开文件、诊断和运行结果均来自 MCP，未以命令行分析冒充。

新增回归测试在普通全套 CTest 中启用，修复前预期会失败。原有七套配置 / ASan / Fuzz 是实现者先前证据；本轮独立验收只复跑上述 Release 及 IDE Debug 定向测试，未重复全部矩阵。下一步修复这两项并复跑回归，再申请 M2 验收；不进入 M3。验收中沙盒失败的两棵自建临时测试树已清理，定向测试自行清理其数据；原有用户改动未清理、未覆盖。

## 修复记录（2026-11-11，待用户复验）

策略与信任边界见 [ADR-0002 §8.4](decisions/0002-m2-store-archive-constraints.md#84-暂存树发布完整性与取消验收修复2026-11-11)。

- **P1**：暂存目录创建时即带受保护私有 DACL（当前用户 / SYSTEM / Administrators），挡住其他主体整个窗口内的写入；改名前再对整棵暂存树复核：目录精确清单、无命名流，文件身份、链接数 1、大小、整文件 CRC32C。改动：`core/filesystem/safe_output.cpp`（`init_staging_security`、`require_exact_children`、`verify_owned_file`、自有目录加 `FILE_LIST_DIRECTORY`）、`safe_output.hpp`、`win_common.hpp`（`nt_relative` 可传安全描述符）、`core/store_archive.cpp`（`verify_staged_tree`）、`core/CMakeLists.txt`（显式链接 advapi32）。
- **P2**：`run_pipeline` 入口与循环后、`ExtractSink::commit` 复核中与改名前检查取消。
- **回归测试** `tests/store_acceptance_tests.cpp` 扩充为：改写已提交文件、外来条目、命名流、原验收组合场景，均须 `integrity_error` 且目标不出现；未篡改解压逐字节一致且发布树只含私有 DACL；空归档 / 仅空文件归档可正常解压；两类归档的预取消 extract / verify 均返回 `cancelled` 且无残留。没有删除或放宽原有用例。

### 实际验证

- MSVC Release、MSVC Debug、GCC / Clang 各 Debug / Release：构建 0 警告，CTest 均 **25/25**（日志 `build/m2-fix-matrix.log`）；clang-format 后 Release 再次 25/25。
- MSVC ASan Release：首次运行因 ASan 运行时不在 PATH 全部 0xc0000135（环境问题）；把 VC Tools 14.51.36231 `Hostx64\x64` 加入 PATH 后 **25/25**，无 ASan 报告。
- `clang-format --dry-run --Werror` 与 `clang-tidy`（clang-ucrt64-debug 编译库）对改动文件通过。
- CLion MCP（errorsOnly=false，逐个打开后查询，均完整返回）：store_archive.cpp、safe_output.cpp、safe_output.hpp、store_acceptance_tests.cpp 无 ERROR。采纳 `verify_staged_tree` 可为 const；其余 WARN 为既有判断过的 `<cstring>`、NT 结构初始化、placement new、stop_token 按值传递，未改。

### 未验证 / 限制

- 未重跑 Fuzz（索引解析未改动）；未测量额外一遍读取的耗时（Store 无基准）。
- 受信主体在复核之后、改名之前的修改仍不在保证内；发布树不继承父目录 ACL（行为变化，用户 2026-11-11 已确认接受）。
- 受限令牌 / 沙盒账户下私有 DACL 的表现未测。
