# Unicode 路径规则验证记录（DEV-015，ADR-0005）

日期：2026-11-14（中途暂停过，当天恢复）。执行者：协调会话。**状态：实现与验证完成，待用户验收，未提交**。改动在 Windows 主检出的工作区里，基于 `3ddc66e`。最终代码的验证结果见 §2.1；§2 的表是暂停前在中间版本上的结果，保留作为过程记录。设计见 [ADR-0005](decisions/0005-portable-unicode-path-profile.md)，数据来源见 [third_party/unicode-ucd/UPSTREAM.md](../third_party/unicode-ucd/UPSTREAM.md)。

## 1. 改动范围（未提交）

| 类别 | 文件 |
| --- | --- |
| 新增实现 | `core/filesystem/unicode_profile.{hpp,cpp}`；`unicode_tables_gen.py`（生成器，只用标准库）；`unicode_tables.inc`（生成结果，约 156 KB） |
| 改动实现 | `core/filesystem/archive_path.cpp`：改用跨平台规则，删除 Windows 和 ASCII 两个分支；`core/include/seedarc/archive_path.hpp` 注释；`core/filesystem/safe_output_posix.cpp` 注释；`core/CMakeLists.txt`：新增源文件，去掉 `normaliz` |
| 测试 | 新增 `tests/unicode_tests.cpp`（`unicode.conformance`）和 CTest `unicode.tables_current`。改动 `path_tests.cpp`、`store_archive_tests.cpp`（`unicode_profile_tests` 取代 Linux 上的非 ASCII 拒绝测试，Linux 的夹具改用真实的 Unicode 名字）、`codec_tests.cpp`、`seed_path_override_tests.cpp`、`check_cli.cmake`（`posix_refusals` 删去非 ASCII 用例）、`tests/CMakeLists.txt` |
| 研究工具 | `research/tools/unicode_win_diff.cpp`（只在 Windows 上构建，与 Win32 API 做差分）、`research/tools/CMakeLists.txt` |
| 第三方数据 | `third_party/unicode-ucd/17.0.0/*`（7 个文件，下载已获用户同意）、`UPSTREAM.md`；`third_party/README.md`；`.gitattributes` 新增 `/third_party/unicode-ucd/** -text` |
| 文档 | ADR-0005（新增）；ADR-0001、ADR-0004 中相关的旧条目标注由 ADR-0005 取代；本记录 |

## 2. 已运行的验证

下表的结果都是在**最后两处小改动之前**的代码上取得的；最终代码的结果见 §2.1。

| 项目 | 代码版本 | 结果 |
| --- | --- | --- |
| Windows 全矩阵：MSVC / GCC UCRT64 / Clang UCRT64 的 Debug 和 Release，加 MSVC ASan Release（`build/msvc-asan-uv`）；每套 `--fresh`、`--clean-first` | 工作区（09:23 开始那一轮；store 测试已修好、已格式化） | 各 39/39，0 警告，没有 ASan 报告，`.sandbox-test` 为空。日志 `build/unicodeverify-*.log` |
| Linux 全矩阵：GCC / Clang 的 Debug 和 Release，加 ASan+UBSan；WSL 分离工作树 `~/SeedArc-unicode` | 快照 `ab87767`（取自 `refs/wip/unicode`） | 各 40/40，0 警告，没有 sanitizer 报告。日志 `~/SeedArc-unicode/build/unicode-*.log` |
| `unicode.conformance` | 同上 | 通过：`NormalizationTest.txt` 的全部用例都满足 c2 = NFC(c1) = NFC(c2) = NFC(c3)、c4 = NFC(c4) = NFC(c5)；Part 1 以外的所有码位 NFC 不变；另做了已分配、组合类、简单折叠和 UTF-8 边界的抽查 |
| `unicode.tables_current` | 同上 | 通过。负对照：在 `.inc` 副本末尾加一行后，检查返回 1 |
| Linux libFuzzer `clang-linux-fuzz`（ASan+UBSan），每个目标 300 s，另加 5 个 NFC / NFD / 大小写 / 未分配 / jamo 种子 | 快照 `ab87767` | path 1,346,805 次、峰值 RSS 503 MB；archive 8,968,400 次、峰值 RSS 520 MB；0 崩溃，0 artifact。语料在 `~/fuzz-work/unicode-20261114T173310Z` |
| 与 Win32 差分（`seedarc_unicode_win_diff`，本机 Windows 10 19045） | 工作区 | 见 §4 |
| CLion（`errorsOnly=false`）：`unicode_profile.cpp`、`archive_path.cpp`、`unicode_tests.cpp`、`path_tests.cpp` | 工作区 | 没有 ERROR。两条 WARN 已改，见 §3。保留的 WARN：`decode_utf8` 中 `length` / `value`“须先重新赋值”，实际上每个分支都会赋值或直接返回，是风格类提示 |
| clang-format 22.1.8 `--dry-run --Werror`，覆盖全部改动的 C++ 文件 | 工作区（含 §3 两处改动） | 通过 |

修过的问题：第一次跑 Linux 时 `archive.store_round_trip` 失败。原因是测试试图在 Linux 上创建 258 字节的文件名，Linux 不允许（`NAME_MAX`），夹具没建成，后面又连带失败了几条。现在输入一侧的超长段只在 Windows 上测，归档一侧在两个平台都测。修改后 Linux 全部 40/40。

### 2.1 最终代码（恢复后，2026-11-14 18:50 起）

最终代码包含 CLion 诊断后改的两处：(a) `unicode_profile.cpp` 的 `tables` 结构移进匿名命名空间；(b) `archive_path.cpp` 改用 `std::views::keys` 遍历。开始前已删除暂停时留下的 `.sandbox-test/m2-memory-59952-*`。

| 项目 | 代码版本 | 结果 |
| --- | --- | --- |
| Windows 全矩阵（与上面相同的七套配置，`--fresh`、`--clean-first`） | 工作区（= 快照 `457a2d4`） | 各 39/39，0 警告，没有 ASan 报告，`.sandbox-test` 为空。日志 `build/unicodeverify-*.log` |
| Linux 全矩阵（五套） | 快照 `457a2d4` | 各 40/40，0 警告，没有 sanitizer 报告；`unicode.tables_current` 为最新；`.sandbox-test` 为空 |
| Windows `msvc-fuzz`（构建 0 警告），三个目标各 300 s，`-seed=20261115` | 工作区 | header 6,529,444 次 / 549 MB；path 666,015 次 / 636 MB；archive 4,183,952 次 / 763 MB；0 崩溃，0 artifact。语料在 `build/msvc-fuzz/unicode-20261115T030354Z` |
| 20 次重复：`unicode.conformance`、`path.boundaries`、`seed.path_rank_overrides`、文件系统与往返各项、`cli.round_trip`、`cli.codec_round_trip`（Linux 另加 `cli.posix_refusals`；Windows 另加 `codec.dictionary_research`） | 同上 | Windows：MSVC Release 和 MSVC ASan 各 12 项 × 20 轮，全部通过，没有 ASan 报告。Linux：Clang Release 和 ASan+UBSan 各 12 项 × 20 轮，全部通过，没有 sanitizer 报告。两边结束后 `.sandbox-test` 都为空 |

Linux 的 path / archive fuzz（§2）是在 `ab87767` 上跑的。到最终代码只改了上面两处，都不影响行为，所以没有在最终代码上重跑 Linux fuzz。

## 3. 暂停经过与仍未完成的

- 暂停时（已在 §2.1 解决）：最后两处改动只通过了 clang-format，Windows 增量构建和 Linux 基于 `4dd0538` 的矩阵都是中途停下的。恢复后改用最终代码完整重跑。
- **一处没查明的残留（仍未解决）**：Linux 第二轮矩阵结束时，`.sandbox-test` 里留下了 `m2-memory-31072-*/large.bin`（0 字节）。它的创建时间正好是工作树重建的那一刻（09:18:20），早于任何测试开始。清理后重跑 gcc-release 和 ASan 两套，都是 40/40，没有残留，未能复现，来源不明。
- **暂停时强制停止留下的文件**：Windows 主检出的 `.sandbox-test/m2-memory-59952-49718560698300/` 里有 `large.bin`（64 MiB 输入）和 `.seedarc-*.tmp`（写到一半的归档，约 19 MB），是被停掉的 bounded-memory 测试留下的。进程被强制结束时，按 ADR-0002 / ADR-0004 的说明会留下这类文件，这不是测试失败。目录被 Git 忽略。恢复时已删除。
- 之后在最终代码上又跑了两套全矩阵和两边的 20 次重复，`.sandbox-test` 都为空，上面那处 Linux 残留没有再出现。
- 没有做 xfs / btrfs 和旧内核的复测。规则只改了名字校验，没有改文件系统代码。
- CLion 补查（2026-11-14，用户重新打开 CLion 之后，`errorsOnly=false`，每个文件先打开再查）：最终版的 `unicode_profile.cpp`、`archive_path.cpp`，以及 `store_archive_tests.cpp`、`research/tools/unicode_win_diff.cpp`。四个文件都没有 ERROR。匿名命名空间和 `views::keys` 两条 WARN 已经消失。剩下的 WARN 只有 `decode_utf8` 中 `length` / `value`“须先重新赋值”两条，理由见 §2；其余都是 INFO。至此本任务涉及的 C++ 文件都已做过 IDE 检查，`*_posix.cpp` 除外（Windows IDE 的配置里看不到它们的代码）。
- Linux clang-tidy 已补做：`archive_path.cpp` 和 `unicode_profile.cpp` 0 条诊断，有探针对照，详见 [L2 / L3 记录 §9](linux-l2l3-validation.md)。没有在服务器上跑长时间测试。
- 那处 Linux 残留做过一次限时排查（2026-11-14）：WSL 虚拟机已经重启，目录名里的单调时钟值无法再换算成墙钟时间，原来的进程信息也没有了，**无法确定原因**。之后所有运行都是干净的。在服务器和后续运行中会继续留意。

## 4. 与 Win32 的差分（本机 Windows 10 19045，只作参考，不作断言）

```
profile UCD 17.0.0
assigned: is_nfc(c) differs from IsNormalizedString                  0
assigned, not private use: is_nfc(c U+0301) differs                  3  U+1AEB U+1E4EC U+1E4ED（Windows 判为 NFC）
private use: is_nfc(c U+0301) differs                              850
unassigned in profile, but Windows calls the text NFC           814664  （新规则全部拒绝）
collide in profile, not in CompareStringOrdinal                    539  U+00B5 U+017F U+01C5 U+01C8 U+01CB U+01F2 U+0345 U+037F …
collide in CompareStringOrdinal (BMP upper), not in profile          0
```

最后一行只覆盖“每个 BMP 码位与它的 `CharUpperBuffW` 结果”这种配对，没有穷举任意两个码位。完整输出在 `build/unicode-win-diff.txt`。

## 5. 剩余步骤

1. 提交、推送须由用户决定。
2. 提交后删除临时引用 `refs/wip/unicode`（Windows 主仓库，最后指向 `457a2d4`）和 `refs/remotes/win/wip-unicode`（WSL），以及 WSL 工作树 `~/SeedArc-unicode`。
