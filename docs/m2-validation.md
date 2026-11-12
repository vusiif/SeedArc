# M2 验证与交接：Store 归档往返

日期：2026-11-11。状态：**P1 / P2 修复后独立复验通过，M2 完成**（见 [M2 验收记录](m2-acceptance.md)“第二轮独立复验”，当前状态以 [STATUS.md](../STATUS.md) 为准）。下文保留实现者原验证证据；最新复验代码为 `4c547b2`，Release 与 ASan 均 25/25，CLion Debug 定向回归通过。设计约束与字节布局见 [ADR-0002](decisions/0002-m2-store-archive-constraints.md)（第 8 节为实现修订），基础容器见 [ADR-0001](decisions/0001-experimental-container-and-output.md)。格式仍为 experimental、未冻结。

## 交付内容

| 模块 | 文件 | 内容 |
| --- | --- | --- |
| 索引 | `core/include/seedarc/archive_index.hpp`、`core/archive_index.cpp` | 条目 / 块记录的小端线格式、`encode_index`、完整校验的 `parse_index`、规范顺序 `archive_path_less` |
| 头部 | `core/archive.cpp`、`archive.hpp` | 增量 `Crc32c`；`validate_header` 公开供索引解析复用（逻辑未变） |
| 安全输出 | `core/filesystem/safe_output.cpp`、`safe_output.hpp`、`win_common.hpp` | 流式会话 `SafeOutputFile`（追加、增量 CRC、读回校验、无替换提交）、暂存目录 `create_staging_child` / `commit_staged`、按卷 + 文件 ID 核对的 `open_owned_child` / `remove_owned_child`；`write_new_file` 改为基于会话 |
| 输入 | `core/filesystem/input_tree.{hpp,cpp}`（内部） | 句柄相对枚举、拒绝重解析点与跨卷、按身份排除输出归档、读取前后变化检测 |
| 管线 | `core/include/seedarc/store_archive.hpp`、`core/store_archive.cpp` | `compress_store`、`verify_archive`、`verify_archive_bytes`、`extract_archive`；verify 与 extract 共用 `run_pipeline` |
| CLI | `cli/main.cpp` | `compress` / `verify` / `extract`、退出码 0–5、Ctrl+C 取消、宽字符参数（非 ASCII 路径） |
| 测试 | `tests/index_tests.cpp`、`store_archive_tests.cpp`、`store_memory_test.cpp`、`check_cli.cmake`、`win_test_support.hpp` | 见下 |
| Fuzz | `fuzz/archive_fuzz.cpp`、`fuzz/corpus/archive/`、`fuzz/make_corpus.py` | 整包内存管线 + 修复 CRC 的索引解析；语料由独立 Python 编码器生成 |

未引入第三方依赖；`third_party/` 无变化。未改动已验收的头部字节布局与默认限额。

## 实际运行的验证

### 构建与 CTest（2026-11-11，全量重建后）

脚本依次执行每个 preset 的 configure / build / ctest，日志 `build/m2-matrix.log`：

```powershell
$env:SEEDARC_UCRT64_ROOT = 'C:/msys64/ucrt64'
cmake --preset <preset>; cmake --build --preset <preset> --parallel; ctest --preset <preset>
# ASan（PATH 前置 VC Tools 14.51.36231 的 bin/Hostx64/x64 以加载 ASan DLL）
cmake -S . -B build/msvc-asan -G 'Visual Studio 18 2026' -A x64 '-DCMAKE_CXX_FLAGS=/EHsc /fsanitize=address /Zi' '-DCMAKE_EXE_LINKER_FLAGS=/DEBUG /INCREMENTAL:NO'
cmake --build build/msvc-asan --config Release --parallel; ctest --test-dir build/msvc-asan -C Release --output-on-failure
```

| 配置 | 构建 | 警告 / 错误行 | CTest |
| --- | --- | --- | --- |
| msvc-vs2026-debug / release | 通过 | 0 | 24/24 / 24/24 |
| gcc-ucrt64-debug / release | 通过 | 0 | 24/24 / 24/24 |
| clang-ucrt64-debug / release | 通过 | 0 | 24/24 / 24/24 |
| msvc-asan（Release，`/fsanitize=address`） | 通过 | 0 | 24/24，无 ASan 报告 |

- 24 项 = 原 18 项中保留的 15 项 + 新增 9 项：`index.boundaries`、`archive.store_round_trip`、`archive.store_bounded_memory`、`fuzz.replay.archive`、`cli.compress_usage` / `extract_usage` / `verify_usage` / `verify_missing` / `round_trip`。原 `cli.compress` / `cli.extract` / `cli.verify`（断言“未实现”）随功能实现删除；`search` / `benchmark` 仍断言退出码 3。
- `clang-format --dry-run --Werror`（core / cli / tests / fuzz 全部 .cpp / .hpp）通过；`clang-tidy --config-file=.clang-tidy -p build/clang-ucrt64-debug` 覆盖全部 20 个 .cpp，退出 0、无诊断。
- 每轮测试后 `.sandbox-test/` 为空。

### 往返（`archive.store_round_trip`，逐字节比较整棵树）

空目录（归档恰为 96 字节头部）、空文件、单文件多块、分块边界（1、4095、4096、4097、8192、3×4096+17 字节，块大小 4096；另有块大小 1）、嵌套目录与空目录（含嵌套空目录）、`a.txt` 与 `a/` 并存的排序情形、非 ASCII 名称（中文目录与文件、NFC 的 é、代理对表情符号）、带空格名称、1200 个小文件 / 12 个目录、2.5 MiB 高熵数据（默认 1 MiB 块，3 块）。另验证：同一输入重复压缩归档逐字节相同；归档写在输入树内部时不把自己打进去；归档同时经文件与内存两条入口校验。

### 负面与恶意归档

每个用例都同时走 `verify_archive_bytes`、`verify_archive`、`extract_archive`，要求错误码一致，且解压目标不出现、无临时残留。

- 截断：0、50、96、106 字节，截去整个索引，截去最后 1 字节；尾部多 1 字节；魔数错误 → `malformed_archive`。
- 损坏：头部、数据首 / 末字节、索引中部、索引 CRC 各翻转一位 → `integrity_error`；块描述符 CRC 与数据不符 → `integrity_error`。
- 路径：`../evil`、`sub/../../evil`、`/evil`、`C:evil`、`evil:stream`、`nul`、`a\evil`、`evil.` → `unsafe_path`，并确认沙盒内外均未出现 `evil`；大小写冲突（`File` / `file`）→ `unsafe_path`。
- 结构（`index_tests`，纯解析，带消息片段断言以证明命中的是目标检查）：乱序 / 重复条目、缺失父目录、文件被当作目录、块数与大小不符、悬空 / 共享 / 越界块引用、无主块、块范围重叠与空洞、非末块不足一块、存储长度与原始长度不符、未知编码器、目录带数据、头部总量 / 块数 / 条目数与索引不符、未知条目类型、条目与块的保留字节非零、路径长度为 0 或越界、非法 UTF-8。
- 限额（读、写两侧各 6 项）：条目数、块数、总输出、数据字节、索引字节、块大小，均为“恰好等于上限通过、上限减 1 报 `resource_limit`”；写出侧超限时不产生归档。
- 冲突：已存在的归档不被覆盖且内容不变；已存在的目录 / 文件不能作为解压目标且内容不变；输出父目录不存在时不创建。

### 输入侧拒绝

输入树内的 junction、以 junction 作为输入根 → `unsupported_feature`（不跟随、不跳过，整次压缩失败，junction 目标未被改动）；名称含 `~`（路径规则不可表示）→ `unsafe_path`；被其他句柄以写方式打开的文件 → `io_error`，写方关闭后可正常压缩；枚举之后文件被追加内容 → `io_error`（消息含 changed）。以上均不留下归档或临时文件。

### 故障注入与取消（复用 M1 的线程局部钩子）

- 解压：已提交两个小文件后在大文件中途写失败、文件中途取消、开始前取消、flush 失败、读回不一致、文件提交失败 —— 均返回对应错误码，目标目录从未出现，暂存目录及其中已提交的文件被逐项清理。短写（每次最多 7 字节）下结果仍逐字节正确。
- **只清理自己创建的对象**：解压中途向暂存目录放入一个外来文件再注入失败 —— 本次创建的文件全部被移除，外来文件保留且内容不变，错误信息含 `cleanup incomplete`，目标目录不出现。
- 压缩：中途写失败、取消、提交失败、读回不一致 —— 无归档、无临时文件。
- 原语：暂存目录提交 / 丢弃 / 重复提交 / 最终名已存在（含大小写不同）；会话大小不符、失败后保持关闭、未提交即析构时删除临时文件；按身份删除在身份不符或类型不符时拒绝且不改变任何对象。
- M1 的 `filesystem.safe_output` 全部断言在 `write_new_file` 改为基于会话后仍通过。

### 有界内存（`archive.store_bounded_memory`）

64 MiB 单文件经 compress → verify → extract，测试自身也分片生成与比对，不持有整文件。断言峰值工作集增长不超过 16 MiB（ASan 构建只报告不断言）。MSVC Release 直接运行的一次结果：

| 阶段 | 耗时 | 吞吐 |
| --- | --- | --- |
| compress | 1.88 s | 34.0 MiB/s |
| verify | 0.64 s | 99.7 MiB/s |
| extract | 1.93 s | 33.2 MiB/s |

峰值工作集：操作前 5.01 MiB，操作后 6.38 MiB（增长约 1.4 MiB，与 1 MiB 块缓冲相符）。ASan 构建为 47.0 → 52.9 MiB。

这是**单次运行的功能性观测，不是基准**：未重复、未控制缓存与磁盘状态，不用于任何性能结论。它给出的定性信息是：verify 只做一遍 CRC32C，而 compress / extract 各做三遍（块 CRC、会话增量 CRC、提交前读回 CRC），耗时约为三倍，即当前瓶颈是逐位实现的 CRC32C，读回校验本身的 I/O 不是主要成本。是否加速 CRC32C、是否保留读回，留给有正式基准的阶段决定（ADR-0002 第 3 节）。

同一机器上 1200 个小文件的完整往返（压缩 + 校验 + 解压 + 比对）约 1.2 s。

### 线格式交叉验证

`fuzz/make_corpus.py` 按 ADR 文字独立实现编码（Python，不共享 C++ 代码）。其生成的空归档、单文件归档、块大小为 4 的小树归档被 `seedarc verify` 接受，`seedarc extract` 解出的内容与脚本输入一致（含中文文件名）；截断样本被拒绝。首次运行时脚本把块记录保留区写成 8 字节而被读取端以“索引含未声明字节”拒绝，属脚本笔误，已按规范的 12 字节修正 —— 也说明读取端对记录长度是严格的。

### Fuzz

- 工具：MSVC 19.51（VC Tools 14.51.36231）`/fsanitize=fuzzer /fsanitize=address`，`msvc-fuzz` preset；库带 inline-8bit-counters / edge / trace-cmp / trace-div 插桩。
- 新目标 `archive`：对任意字节运行整包内存管线（头部 → 定位并解析索引 → 逐块 CRC 解码），接受时断言索引可逐字节重新编码、条目有序、块归属与总量一致；另一路把输入视为“16 字节参数 + 索引记录”并修复 CRC，使变异能到达记录级校验。
- 命令（PATH 前置 VC Tools 的 `bin/Hostx64/x64`）：

```powershell
cmake --preset msvc-fuzz
cmake --build --preset msvc-fuzz --parallel
build/msvc-fuzz/fuzz/Release/seedarc_fuzz_<t>.exe build/msvc-fuzz/work-m2/<t> fuzz/corpus/<t> -max_total_time=<秒> -max_len=4096 -timeout=10 -rss_limit_mb=2048 -seed=20261111 -print_final_stats=1 -artifact_prefix=build/msvc-fuzz/artifacts-m2/<t>/
```

| 目标 | 时长 | 执行数 | exec/s | 新增单元 | 结束语料 | 峰值 RSS | 崩溃 / artifact |
| --- | --- | --- | --- | --- | --- | --- | --- |
| archive（新） | 241 s | 4770840 | 19796 | 466 | 143 / 35 KB，ft 3437 | 707 MB | 0 |
| header（库改动后重跑） | 61 s | 1840428 | 30170 | 267 | 89 / 28 KB，ft 856 | 554 MB | 0 |
| path（库改动后重跑） | 61 s | 348456 | 5712 | 1724 | 456 / 33 KB，ft 3671 | 632 MB | 0 |

- 日志 `build/msvc-fuzz/<t>-m2-run.log`，生成语料在 `build/msvc-fuzz/work-m2/`（不入库）。种子语料 `fuzz/corpus/archive/`（5 个，由 `fuzz/make_corpus.py` 生成）已入库，并由普通构建的 `fuzz.replay.archive` 重放。
- 这是**短时运行，不是持续 Fuzz**；不覆盖文件系统写出路径（那部分由故障注入与真实目录测试覆盖）。

## 未验证 / 限制

1. **原实现会话未取得 CLion 诊断**：服务当时连接失败；后续独立验收已在用户协助置前台后补查四个核心文件和新增回归测试，无 ERROR，保留相关风格 / 分析器提示，范围与判断见 [验收记录](m2-acceptance.md)。这不替代所有其他文件的 IDE 检查。
2. **解压到已有目录未实现**：目标已存在一律拒绝（ADR-0002 第 8.3 节）。
3. **可用磁盘空间预检未做**；真实磁盘写满只由注入的 `ERROR_DISK_FULL` 模拟，未在真实满盘上测试。
4. Fuzz 为短跑，不是持续 Fuzz；MSYS2 Clang / GCC 的 fuzzer 运行时仍未验证。
5. 未测试：真实 SUBST 盘符（沿用 M1 限制）、文件符号链接作为输入（创建需要特权，目录 junction 已覆盖重解析点拒绝路径）、接近 8 GiB 上限的真实大归档、10 万条目规模、非 NTFS / 网络卷（按设计拒绝，未实测）、Windows 之外的平台（按设计 fail closed，未构建）。
6. 索引解析的常驻内存上界（索引副本 + 路径冲突检查用的宽字符表）未单独测量，只受 `max_index_bytes` 与 `max_entries` 约束。
7. 进程被强杀时可能残留 `.seedarc-<hex>.tmp` 文件或目录，不做崩溃恢复。
8. 性能数字不是基准；没有运行压缩率实验（Store 不压缩，本阶段无意义）。

## 下一步

M3：接入 Zstd 与 LZMA2（vcpkg manifest、`core/codec/` 适配层、逐算法往返与畸形输入测试）。块记录已有 `codec_id` / `codec_config_id` 与分离的 stored / raw 长度，读取端目前只接受 Store。
