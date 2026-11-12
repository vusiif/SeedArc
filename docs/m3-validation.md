# M3 验证记录：Zstandard / LZMA2

日期：2026-11-11。任务 DEV-004；设计与取舍见 [ADR-0003](decisions/0003-m3-zstd-lzma2-codecs.md)。结论：**M3 实验阶段独立验收通过（2026-11-12）；IDE 覆盖限制见文末**。本次交付包含实现、验收证据与 CLion 配置修复说明。

用户想法目录文件清单及两份 SHA256 与 STATUS 快照一致，无新增输入。

## 变更范围

| 部分 | 文件 | 内容 |
| --- | --- | --- |
| 依赖 | `vcpkg.json`、`vcpkg-configuration.json`、`CMakePresets.json`、`core/CMakeLists.txt` | zstd 1.5.7、liblzma 5.8.4，git registry baseline `9eec002…`（tag 2026.10.16）；隐藏 presets `vcpkg-msvc` / `vcpkg-mingw` |
| codec 边界 | `core/codec/codec.hpp`、`core/codec/codec.cpp` | 唯一包含第三方头文件的位置；RAII 上下文；显式参数；严格的单流 / 长度 / 窗口 / 内存检查 |
| 归档 | `core/archive.cpp`、`core/store_archive.cpp`、`core/include/seedarc/store_archive.hpp` | 块 codec 规则；按块压缩 + Store 回退 + 写前自检；解码管线；`compress_store` → `compress_archive`；`Codec` / `level` 选项 |
| 安全写出 | `core/filesystem/safe_output.cpp`、`safe_output.hpp` | `reserve_prefix` / `write_prefix`：头部回填，提交时逐字节核对 |
| CLI | `cli/main.cpp` | `--codec`、`--level`；`--version` 输出库版本 |
| 测试 | `tests/codec_tests.cpp`（新）、`index_tests.cpp`、`store_memory_test.cpp`、`check_cli.cmake`、`CMakeLists.txt` | 见下 |
| Fuzz | `fuzz/archive_fuzz.cpp`、`fuzz/corpus/archive/{zstd,lzma2}-tree.bin` | 接受时断言 codec 长度规则；两份由本库 CLI 生成的压缩块种子 |

## 构建与 CTest（全部 `--fresh` 重新配置，日志 `build/m3-matrix.log`）

```powershell
$env:VCPKG_ROOT = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg'
$env:SEEDARC_UCRT64_ROOT = 'C:/msys64/ucrt64'
cmake --preset <preset> --fresh; cmake --build --preset <preset> --parallel; ctest --preset <preset>
```

| 配置 | 构建 | CTest |
| --- | --- | --- |
| msvc-vs2026-debug / release | 0 警告 | 30/30 |
| gcc-ucrt64-debug / release | 0 警告 | 30/30 |
| clang-ucrt64-debug / release | 0 警告 | 30/30 |
| msvc-asan（Release，`/fsanitize=address`，vcpkg 工具链） | 首轮 1 条 C4189（ASan 分支未用 `allowed_growth`），已修复后重建 0 警告 | 30/30，无 ASan 报告 |

clang-format 应用后又重建 MSVC Release（30/30）、GCC Release、ASan（30/30）。`clang-format --dry-run --Werror` 覆盖全部改动的 C++ 文件；`clang-tidy -p build/clang-ucrt64-debug` 覆盖 codec.cpp、archive.cpp、store_archive.cpp、safe_output.cpp、cli/main.cpp、codec_tests.cpp，退出 0、无诊断。

30 项 = M2 的 25 项 + `codec.round_trip`、`codec.zstd_bounded_memory`、`codec.lzma2_bounded_memory`、`cli.codec_usage`、`cli.codec_round_trip`。

## 覆盖内容

- **codec 边界**（`codec.round_trip` 前半）：链接版本与 manifest 一致；非法 codec / 级别被编码器拒绝；zstd 1 / 19、LZMA2 0 / 9 往返且编码确定；截断、尾随字节、输出偏短 / 偏长、空输入均为 `integrity_error`；翻转字节要么失败、要么解出不同内容（由块 CRC 兜底）；随机数据报告“不更小”；手工构造的 zstd 帧：合法帧可解、无内容大小被拒、声明 1 GiB 窗口被拒（`resource_limit`）、两帧连接被拒、跳帧被拒；手工 LZMA2 未压缩块：缺结束标记被拒、补上后可解；未知 codec id 被解码器拒绝。
- **归档**（后半）：Store、zstd（默认 / 1 / 19）、LZMA2（默认 / 0 / 9）对含多块文本、整分块文件、随机数据、混合文件、空文件、空目录、非 ASCII 名称的树逐字节往返；索引中每块记录的 codec / 级别与实际一致，文本块被压缩、随机块回退为 Store；压缩归档小于 Store；同一构建内重复压缩字节相同；非法选项不写出任何东西；1 字节分块全部回退仍可往返；压缩块首字节 / 中间字节损坏时 verify 与 extract 均为 `integrity_error`，目标不出现、无残留。
- **索引**（`index.boundaries` 新增）：Store 带配置、zstd 0 / 20、LZMA2 10、codec 3 均 `unsupported_feature`；压缩块不小于原长为 `malformed_archive`；四个边界级别的合法压缩块被接受。
- **CLI**：`--codec` / `--level` 的缺值、非法值、重复、错误命令组合退出 2；四种 codec / 级别经 CLI 往返且归档小于内容；越界级别与 Store 带级别在库内拒绝、不留输出。

## 有界内存（单次功能性观测，不是基准）

64 MiB 低熵输入（16 种字节值），默认 1 MiB 分块，MSVC Release：

| codec | 归档 | 峰值工作集增长 | 断言上界 | compress / verify / extract |
| --- | --- | --- | --- | --- |
| store | 64.0 MiB | 1.4 MiB | 16 MiB | 1.9 / 0.6 / 2.5 s |
| zstd-3 | 33.1 MiB | 4.6 MiB | 16 MiB | 1.6 / 0.7 / 2.5 s |
| lzma2-6 | 33.3 MiB | 14.9 MiB | 32 MiB | 24.3 / 3.2 / 5.6 s |

耗时是一次运行的功能性观测，含解压时的三遍 I/O 与写前自检解码；不能作为性能结论。

## Fuzz

`msvc-fuzz` preset（`--fresh`，vcpkg 工具链）重建全部 fuzz 目标，0 警告。只对 `archive` 目标短跑：

```text
build/msvc-fuzz/fuzz/Release/seedarc_fuzz_archive.exe build/msvc-fuzz/work-m3/archive fuzz/corpus/archive -max_total_time=180 -max_len=4096 -timeout=10 -rss_limit_mb=2048 -seed=20261111 -print_final_stats=1 -artifact_prefix=build/msvc-fuzz/artifacts-m3/archive/
```

181 s、3 028 378 次执行（16 731 exec/s），新增 451 个单元，结束语料 136 个；峰值 RSS 596 MiB（含 ASan / libFuzzer 开销）；无崩溃、无 artifact。日志 `build/m3-fuzz-archive.log`。header / path 目标代码未改，本轮未重跑。

## 跨工具链观察

对同一份输入（`docs/*.md` 的副本），MSVC Release 与 GCC Release 的 CLI 以 zstd / LZMA2 默认级别生成的归档 SHA256 相同。这是一次观察，不构成跨环境确定性保证（M4 范围）。

## CLion 诊断（2026-11-12）

用户重新连接 CLion 并置于前台后补查。`get_compiler_info` 确认 IDE 加载 `clang-ucrt64-debug`：clang++（UCRT64）、`-std=c++23`、`-Wall -Wextra -Wpedantic`，vcpkg `x64-mingw-static` 头文件路径与 `LZMA_API_STATIC` 生效。每个文件先 `open_file_in_editor`，再以 `errorsOnly=false` 调用 `get_file_problems`。

- **探针**：默认参数下 13 个文件均返回空列表；放入一个含未用变量与未定义符号的临时文件，默认参数仍为空，`errorsOnly=false` 才报出 3 条。说明默认查询不含 warning，空结果不能当成无问题；探针已删除。
- **覆盖**：codec.cpp / codec.hpp、archive.cpp、store_archive.cpp、safe_output.cpp / .hpp、store_archive.hpp、cli/main.cpp、codec_tests.cpp、index_tests.cpp、store_memory_test.cpp、store_archive_tests.cpp、store_acceptance_tests.cpp、fuzz/archive_fuzz.cpp，全部完整返回。
- **结果**：无 ERROR。已修复的有效 WARN：
  - `cli/main.cpp`：`--level` 解析里把赋值用作条件，拆成先赋值再判断，行为不变；
  - `codec_tests.cpp`：手工 zstd 块头常量改为 `constexpr`。
- **核实后保留的 WARN**：
  - `Encoder::encode` / `Decoder::decode` “可设为 const”：二者经 pimpl 修改 zstd / liblzma 上下文，标 const 会误导；
  - `stop_token` 按值传参：与公共头文件签名一致，拷贝只是引用计数；
  - `safe_output.cpp` 的 `<cstring>`（`std::memcpy` 在用）和 `store_memory_test.cpp` 的 `<cstdlib>`（`std::strtoull` 在用）“可能未使用”，是误报；
  - `safe_output.cpp` 中 placement new 的 “Potential memory leak”（无堆分配）、零初始化后再赋值的结构体、`digits` 作用域，以及 `index_tests.cpp` 的 `stray` 可 constexpr，均为 M3 之前的代码或误报，本轮不动。
- 其余为 INFO 级风格提示（指定初始化器、形参可 const、冗余 `static_cast` 等）。冗余 cast 是 `uint64_t` → `size_t`，只在 64 位下冗余，保留。

修复后两处文件复查无 WARN，`clang-format --dry-run --Werror` 通过；MSVC Release、Clang UCRT64 Debug、GCC UCRT64 Release 增量构建 0 警告、CTest 30/30（日志 `build/m3-clion-fix.log`）。其余四套配置与 ASan 未针对这两处改动重跑。

## 未验证 / 限制

- Clang ASan、UBSan 未运行（MSVC 工具链仅 ASan）；Fuzz 仅短跑一个目标。
- 未测量性能，未做公平基准（M5）；Store 回退的收益只按“块记录定长、无额外索引成本”论证。
- 发布时需附带 zstd / liblzma 许可文本，未做。

## 独立验收（2026-11-12）

结论：**M3 实验阶段通过**。本轮保留全部原实现与测试断言，只更新状态和本记录；无提交、推送。用户想法文件清单及两份 SHA256 与 STATUS 一致。

### 独立执行证据

- `cmake --build --preset msvc-vs2026-release --parallel`，随后 `ctest --preset msvc-vs2026-release --output-on-failure`：30/30，51.02 s，构建无警告。日志 `build/m3-acceptance-release-{build,test}.log`。
- `cmake --build build/msvc-asan --config Release --parallel`，随后 `ctest --test-dir build/msvc-asan -C Release --output-on-failure`：30/30，60.16 s，构建无警告、无 ASan 报告。运行时 PATH 加入实际 VC Tools 14.51.36231 的 `bin/Hostx64/x64`；缓存确认 `/fsanitize=address` 与 vcpkg 工具链生效。日志 `build/m3-acceptance-asan-{build,test}.log`。
- 文件系统测试均在正常用户身份执行，避免已确认的沙盒祖先 ACL 干扰；未放宽安全检查。
- clang-format `--dry-run --Werror` 覆盖 14 个 M3 相关 C++ 文件，通过；`git diff --check` 通过。`.sandbox-test` 无残留。
- 主线程及只读子审查 `m3_review` 核对编码参数、受限解码、Store 回退、写前逐字节自检、头部回填与提交复核，未发现可确认的阻塞缺陷。子审查未运行测试，以上测试为主线程独立执行。

### CLion 本轮覆盖

按 jetbrains-connect 技能从现有客户端配置发现服务，并传入项目绝对路径。`get_compiler_info` 确认 Clang UCRT64、`-std=c++23`、vcpkg 头文件与 `LZMA_API_STATIC`。每个文件先打开，诊断均显式 `errorsOnly=false`。

14 个目标文件中，13 个完整返回：codec.cpp / .hpp、archive.cpp、store_archive.cpp、safe_output.cpp / .hpp、store_archive.hpp、cli/main.cpp、codec_tests.cpp、index_tests.cpp、store_memory_test.cpp、store_archive_tests.cpp、fuzz/archive_fuzz.cpp。无 ERROR；WARN 与上文已核实的风格提示和误报一致，拼写提示按用户要求忽略。完整结果 `build/m3-acceptance-ide.json`。

`store_acceptance_tests.cpp` 本轮查询被服务取消，未取得完整诊断；该文件 M3 改动仅调用 API 更名，原实现阶段诊断记录及本轮两套全量动态测试仍保留。现有 IDE 运行配置尝试返回“进程未启动，运行程序未提供失败原因”，未将其记录为通过；日志 `build/m3-acceptance-ide-run.json`。本轮不宣称 IDE 全文件覆盖或 IDE 内运行通过。

### 限制与下一步

本轮未重新生成 Fuzz 输入；全量 CTest 包含语料重放，并核对原实现者的 181 s / 3028378 次 Fuzz 日志。七套工具链矩阵、clang-tidy 与持续 Fuzz 沿用实现者记录，未冒充本轮独立执行。MSVC ASan 不覆盖未插桩的第三方静态库内部；Clang ASan / UBSan 仍未运行。

建议后续为 `reserve_prefix` / `write_prefix` 的缺少回填、回填短写及重复回填补定向状态测试，目前未发现对应实现缺陷，不列为阶段阻塞。格式未冻结，未作正式发布许可打包或跨环境确定性保证；下一阶段为 M4，本轮不启动。

## CLion 默认 Debug profile 修复（2026-11-12）

用户随后提供的实际重新加载日志表明：手工 `Debug` profile 只设置了 Qt 路径，没有 vcpkg 工具链，导致 liblzma 查找失败。之前 MCP 返回的 Clang 编译器快照不足以证明当前启用的 Debug profile 能加载；前述验收的 IDE 可见运行限制不能仅归因于前台焦点。

通过真实 CLion MCP `update_cmake_profile` 修改现有 `Debug`：保留 Qt prefix，补入本机 vcpkg 工具链路径与 `x64-windows-static-md`，构建目录改为 `cmake-build-debug-vcpkg`，保留旧目录。未改全局环境或共享 presets。

- `load_cmake`：`SUCCEEDED` / exitCode 0，依赖从 vcpkg 缓存恢复，配置生成完成，见 `build/clion-m3-config-fix.json`。
- `get_compiler_info`：`libseedarc | Debug`，MSVC 19.51 工具链、`-std:c++latest`、`/Zc:__cplusplus`、`/MDd`，依赖 include 指向本 profile 的 vcpkg 安装目录。
- IDE 现有 `seedarc_cli --version`：exitCode 0，输出 zstd 1.5.7 / liblzma 5.8.4，日志 `build/clion-m3-config-fix-run.json`。
- IDE 现有 `seedarc_codec_tests`，单次参数为项目 `.sandbox-test`：exitCode 0，`all codec checks passed`，日志 `build/clion-m3-config-fix-codec.json`。
- `core/CMakeLists.txt` 以 `errorsOnly=false` 查询，完整返回空诊断。

用户人工补查 `store_acceptance_tests.cpp` 仅有三条拼写 / 英语风格提示，按用户要求忽略；这是用户报告，未记为本轮 MCP 完整返回。M3 实验阶段结论保持通过。
