# Linux L2 / L3 验证记录（DEV-014）

日期：2026-11-13。执行：DEV-014 子会话（协调会话委派；用户决定“先只做 Linux，补到与 Windows 差不多”）。分支：`linux-l2l3`（WSL 克隆 `~/SeedArc`，基于 `linux-l1` 的 `b266e0b`；本地提交，未推送、未合并）。设计：[ADR-0004](decisions/0004-posix-filesystem.md)。前序：[L1 记录](linux-l1-validation.md)。

## 1. 结论

| 验收项 | 结果 |
| --- | --- |
| ADR 先于实现 | ADR-0004 逐条对照 ADR-0001 / ADR-0002 §8.4；做不到的保证一律失败关闭 |
| L2 输入枚举 / L3 安全输出 | 已实现（`core/filesystem/*_posix.cpp`），公共 API 不变；Windows 代码只加 `#if` 隔离 |
| CLI compress / verify / extract | Linux 端到端可用（`cli.round_trip`、`cli.codec_round_trip`、手工往返 `diff -r` 一致） |
| Windows 36 项测试的 Linux 对应 | 36 个同名测试全部在 Linux 实际运行，另加 `cli.posix_refusals`，共 37 项；无 DISABLED |
| GCC / Clang Debug / Release、ASan + UBSan | 5 套各 37/37 通过，`--clean-first` 全量重编 0 警告，无 sanitizer 报告（第 5.1 节） |
| Fuzz | header / path / archive 各 300 s，共约 4045 万次执行，0 崩溃、0 artifact（第 5.3 节） |
| 内存有界 | 64 MiB 输入，峰值 RSS 增长 store 1.5 / zstd 4.9 / LZMA2 14.8 MiB，均在 16 / 16 / 32 MiB 上限内（第 5.4 节） |
| 非 ASCII 文件名 | **按用户选择的方案 A 不支持**：Linux 明确拒绝（`unsupported_feature`），测试改用 ASCII 等价名字并断言拒绝 |

## 2. 环境与能力探测（WSL 实测）

内核 `6.18.33.2-microsoft-standard-WSL2`，glibc 2.43，GCC 15.2.0，Clang 21.1.8，用户 uid 1000（非 root），umask 0022，`protected_hardlinks=1`、`protected_symlinks=1`。探针（不入库）在各文件系统上的结果：

| 位置 | f_type | O_TMPFILE | linkat AT_EMPTY_PATH（无特权） | RENAME_NOREPLACE | openat2 |
| --- | --- | --- | --- | --- | --- |
| `~`（ext4） | 0xEF53 | 支持 | 支持（内核 ≥ 6.10） | 支持，占用时 EEXIST | 支持 |
| `/tmp`、`/dev/shm`（tmpfs） | 0x01021994 | 支持 | 支持 | 支持 | 支持 |
| `/mnt/c`（9p / drvfs） | 0x01021997 | **EOPNOTSUPP** | — | **EINVAL** | 支持 |

据此：9p 被白名单拒绝；`linkat(AT_EMPTY_PATH)` 在旧内核（如服务器 Ubuntu 24.04 的 6.8）上无特权会失败，所以实现带 `/proc/self/fd` 回退，并用测试专用开关 `force_proc_link` 在本机强制走回退路径。

## 3. 代码改动

| 文件 | 内容 |
| --- | --- |
| `docs/decisions/0004-posix-filesystem.md` | 新 ADR：文件系统白名单、能力探测、逐条等价对照、输入枚举、文件名编码、元数据、测试要求、CLI 取消、剩余差异 |
| `core/filesystem/posix_common.hpp` | `Fd`、errno → 错误码、(dev, ino) 身份、文件系统白名单 |
| `core/filesystem/safe_output_posix.cpp` | Linux `SafeOutputDirectory` / `SafeOutputFile` |
| `core/filesystem/input_tree_posix.cpp` | Linux `InputTree` / `InputFile` |
| `core/filesystem/{safe_output,input_tree}.cpp` | 在 Linux 上排除 Windows / 桩实现（`#if defined(_WIN32) \|\| !defined(__linux__)`），内容不变 |
| `core/include/seedarc/safe_output.hpp` | Linux 路径预算 4095 字节（`PATH_MAX - 1`）；Windows 仍为 32000 |
| `core/filesystem/safe_output_internal.hpp` | `WriteFaults::force_proc_link`（仅测试） |
| `core/filesystem/input_tree.hpp` | `components` 改为 `std::filesystem::path::string_type`（Windows 仍是 `wstring`） |
| `cli/main.cpp`、`cli/CMakeLists.txt` | Linux 信号取消（屏蔽信号 + `sigtimedwait` 线程），链接 `Threads::Threads` |
| `tests/safe_output_posix_tests.cpp` | `filesystem.safe_output` 的 Linux 版 |
| `tests/{store_archive,store_acceptance,store_memory,codec,seed_archive,seed_adaptive}_tests.cpp`、`tests/test_fixture.hpp` | 跨平台；Windows 专有断言保留在 `#ifdef _WIN32` 中，Linux 换成等价检查 |
| `tests/CMakeLists.txt`、`tests/check_cli.cmake` | 两平台同名注册；新增 `cli.posix_refusals`，删除 L1 的 `cli.posix_unsupported` |

## 4. Windows 专有断言的 Linux 对应

| Windows 断言 | Linux 对应 |
| --- | --- |
| junction 拒绝（输入内、输入根、输出根、目标已存在） | 目录 / 文件 / 悬空符号链接，任意路径组件；均拒绝且不跟随 |
| 已有硬链接目标拒绝 | 同（`output_conflict`），外部文件不变 |
| 固定的父目录不能改名 | **无对应**：Linux 可以改名；改为断言改名后写入仍进入原目录（fd 语义），不进入新占用该名字的目录 |
| 属性型重解析变更不能重定向写入 | 已创建目录被换成指向外部的符号链接后，写入仍进原目录，`discard_empty` 拒绝删除该链接 |
| 私有 DACL（暂存 / 发布树） | 属主为本人，目录恰好 0700、文件恰好 0600；放宽权限在复核时被检测 |
| NTFS 命名流（复核时检测） | `user.*` 扩展属性（复核时检测）；另增：额外硬链接、同内容替换文件 |
| 暂存名不区分大小写（`FINAL` 与 `final` 冲突） | Linux 区分大小写：断言 `FINAL` 是另一个空闲名字；只差大小写的外来条目仍被复核拒绝 |
| 被其他程序以写方式打开的输入使压缩失败 | **无对应**（POSIX 不能拒绝写者）：断言空闲写者不阻止压缩；真正的修改由 “input changed after enumeration” 测试检出 |
| delete-pending 对象算残留 | POSIX 没有该状态：断言存在的对象是残留、已删除的报告为消失 |
| 卷 GUID 根（SUBST）单元测试 | 无对应（Windows 专有）；改为路径字节预算、文件系统白名单测试 |
| 峰值工作集 | 峰值 RSS（`ru_maxrss`），口径不同 |

Linux 专有负面测试：组 / 其他人可写且无粘滞位的目录被拒（粘滞位 1777 可用）、只读目录写入 / 建目录 / 暂存失败且不留东西、procfs / sysfs / 9p 等不接受的文件系统、输入中的 FIFO（打开不阻塞）、procfs 上的输入、暂存目录在复核后被移走则不发布、发布时目标名已被占用不覆盖、回滚不删除替换品（同名普通文件或链接）、`/proc/self/fd` 发布路径与其不覆盖、非 ASCII 输入名 / 归档路径被明确拒绝。

## 5. 结果

### 5.1 构建与全量测试（最终提交的源码，`--clean-first` 全量重编）

| preset | 编译单元 | 警告 | CTest |
| --- | --- | --- | --- |
| `gcc-linux-debug` | 38 | 0 | 37/37 |
| `gcc-linux-release` | 38 | 0 | 37/37 |
| `clang-linux-debug` | 38 | 0 | 37/37 |
| `clang-linux-release` | 38 | 0 | 37/37 |
| `clang-linux-asan-ubsan` | 38 | 0 | 37/37，无 ASan / UBSan / LSan 报告 |
| `clang-linux-fuzz` | 14 | 0 | —（只构建三个 fuzz 目标） |

格式：改动的 17 个 C++ 文件经 clang-format 22.1.8（Windows UCRT64，项目 `.clang-format`）`--dry-run --Werror` 通过；本表即在格式化后的最终源码上运行。

测试清单（每套 37 项，与 Windows 同名的 36 项 + `cli.posix_refusals`）：core / seed 候选与覆盖 / m3 兼容 / archive / path / index 边界、`seed.archive_integration`、`seed.adaptive_file`、`filesystem.safe_output`、`archive.store_round_trip`、`archive.store_acceptance`、`codec.round_trip`、三项 bounded memory、16 项 CLI（含 `round_trip`、`codec_round_trip`、`posix_refusals`）、三项 fuzz 语料回放。没有 DISABLED 项。

### 5.2 重复运行

`ctest --repeat until-fail:20`，对 `filesystem.safe_output`、`archive.store_round_trip`、`archive.store_acceptance`、`seed.archive_integration`、`seed.adaptive_file`、`codec.round_trip`、`cli.round_trip`、`cli.codec_round_trip`、`cli.posix_refusals` 九项：Clang Release 与 ASan + UBSan 各 20 轮全部通过，`.sandbox-test` 无残留（格式化前的同一逻辑源码）。

### 5.3 libFuzzer（`clang-linux-fuzz`，ASan + UBSan，`-fno-sanitize-recover=undefined`）

三个目标并行各 `-max_total_time=300`；新语料写到 `~/fuzz-work/l2l3-20261114T041955Z/`，仓库语料只读、未改动。

| 目标 | 执行次数 | exec/s | cov / ft | 峰值 RSS | 崩溃 / artifact |
| --- | --- | --- | --- | --- | --- |
| header | 24,445,199 | ≈81k | 248 / 487 | 501 MB | 0 / 0 |
| path | 1,950,622 | ≈6.5k | 366 / 2452 | 573 MB | 0 / 0 |
| archive | 14,058,796 | ≈47k | 903 / 2003 | 487 MB | 0 / 0 |

限制：fuzz 目标只覆盖解析与路径规则，不覆盖 POSIX 文件系统代码（后者由第 4 节的负面测试与 sanitizer 覆盖）。

### 5.4 内存有界（GCC Release，64 MiB 输入）

| 用例 | 归档 | 峰值 RSS 前 → 后 | 增长 | 上限 |
| --- | --- | --- | --- | --- |
| store | 64.00 MiB | 4.36 → 5.91 MiB | 1.5 MiB | 16 MiB |
| zstd-3 | 33.10 MiB | 4.44 → 9.35 MiB | 4.9 MiB | 16 MiB |
| LZMA2-6 | 33.28 MiB | 4.39 → 19.14 MiB | 14.8 MiB | 32 MiB |

口径为 `getrusage(RUSAGE_SELF).ru_maxrss`（峰值常驻集），与 Windows 的峰值工作集不同（Windows MSVC Release 记录为 1.4 / 4.6 / 14.9 MiB），只作同口径内的上界检查，不作跨平台比较。ASan 构建不断言上界。

## 6. 手工检查

- CLI 往返：store / zstd / LZMA2 各一次，`diff -r` 一致；解压目录 0700、文件 0600，归档 0644（umask 0022）。
- 拒绝：已存在的目标、输入中的符号链接与 FIFO、经符号链接的输出父目录、组可写父目录、`/mnt/c`（9p）均退出码 5；粘滞 1777 目录与 tmpfs 成功。
- 取消：120 MB 输入 `--codec lzma2 --level 9` 压缩 2 s 后 `SIGINT` → 退出码 4、无归档；解压 0.3 s 后 `SIGTERM` → 退出码 4、无目标与暂存目录。

## 7. Windows 侧验证（3f8d84c）

2026-11-14，由协调会话执行：把本分支取到 Windows 仓库，记为 `refs/remotes/wsl/linux-l2l3`，在分离的 worktree `C:\Users\IceLolly\Music\Clion\SeedArc-winverify` 中运行，没有动 Windows 主检出。环境：`VCPKG_ROOT` 为 VS Build Tools 自带的 vcpkg，`SEEDARC_UCRT64_ROOT=C:/msys64/ucrt64`。每个 preset 依次运行 `cmake --preset <p> --fresh`、`cmake --build --preset <p> --parallel --clean-first`、`ctest --preset <p> --output-on-failure`。

| 配置 | 警告 | CTest |
| --- | --- | --- |
| msvc-vs2026-debug / msvc-vs2026-release | 0 | 各 36/36 |
| gcc-ucrt64-debug / gcc-ucrt64-release | 0 | 各 36/36 |
| clang-ucrt64-debug / clang-ucrt64-release | 0 | 各 36/36 |
| MSVC ASan（Release，独立目录 `build/msvc-asan-wv`，vcpkg 工具链，`x64-windows-static-md`，CXX `/EHsc /fsanitize=address /Zi`，链接 `/DEBUG /INCREMENTAL:NO`，PATH 加 VC Tools `bin\Hostx64\x64`） | 0 | 36/36，无 AddressSanitizer 报告 |

Windows 上是 36 项，因为 `cli.posix_refusals` 只在 Linux 注册，这符合预期。跑完后 `.sandbox-test` 为空，worktree 的 `git status` 干净。日志在 `C:\Users\IceLolly\Music\Clion\SeedArc-winverify\build\winverify-*.log`，汇总在 `winverify-summary.txt`。

Windows 侧本轮**未运行**：msvc-fuzz、CLion 诊断（MCP 断开）、20 次重复测试。

## 8. 未运行 / 限制

- 只在 WSL（ext4 / tmpfs）上运行；xfs、btrfs 在白名单中但**未实测**；旧内核（< 6.10）上 `AT_EMPTY_PATH` 失败后走 `/proc` 回退这一真实分支未在旧内核上运行（只通过 `force_proc_link` 在新内核上测试回退本身）。
- 测试以普通用户运行；以 root 运行时只读目录、受保护父目录等断言的含义不同（部分断言在 root 下跳过）。
- Windows 侧已在 `3f8d84c` 上验证通过（见 §7），但 msvc-fuzz 和 Windows 上的 20 次重复测试都没有运行。
- CLion 诊断未运行，Linux 和 Windows 两侧都没有（MCP 连接失败）；clang-tidy 也未运行。
- 服务器长测未做；归档格式未改。
