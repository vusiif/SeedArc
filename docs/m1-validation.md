# M1 当前验证与交接

日期：2026-11-11。状态：**安全 I/O 加固项已实现并通过六套配置 + ASan 18/18，头部 / 路径 Fuzz 已实际运行；CLion IDE 诊断仍超时，M1 待验收**。

## 已完成

- 候选 [ADR-0001](decisions/0001-experimental-container-and-output.md)，实验性 96 字节小端头、CRC32C、头部资源限额和 Store 块描述符范围验证。
- Windows 路径 UTF-8 / NFC、组件及清单冲突检查。
- 安全输出单文件 / 新目录原语：相对目录句柄、新建不覆盖、读回验证、按句柄清理、卷身份、累计深度 / 路径预算、故障注入测试。不接入 CLI。
- 用户两份想法哈希未变化（第三轮复核同上次 SHA256）。结构化种子空间只作候选，不冻结位宽 / 语义。

## 第二轮：error 5 定位与提交路径修复

1. **error 5 根因**：上一轮测试在沙盒账户 `WIN10\CodexSandboxOffline` 下运行（`.git` 属主即该账户），该账户对 `C:\Users\IceLolly` 等祖先目录无显式 FILE_TRAVERSE 授权。以实际用户运行同一代码即通过。结论：环境 ACL 限制，不是实现缺陷；未放宽任何访问请求或检查。
2. **随后暴露的真实缺陷**：`SetFileInformationByHandle(FileRenameInfo)` 以父句柄作 `RootDirectory` 时返回 error 87，提交从未成功。
3. **已证伪的危险方案**：kernel32 + `RootDirectory = NULL` 会把文件移动到**进程当前目录**并绕过“已存在目标拒绝”。试验产生的 5 个已知测试文件已逐个删除，方案已回滚。
4. NT 层以父句柄作 `RootDirectory`：`STATUS_SHARING_VIOLATION (0xc0000043)`。
5. **最终方案**：NT 层 `NtSetInformationFile`、`RootDirectory = NULL` + 单组件名，在文件当前所在（固定）目录内重命名；错误信息带 NTSTATUS。
6. 回归保护：测试把进程当前目录切到 `outside`，断言其中只有 sentinel。
7. MinGW 预定义 `NOMINMAX` 的重定义警告：加 `#ifndef` 守卫。

## 第三轮：M1 加固

| 项 | 实现 | 测试证据 |
| --- | --- | --- |
| 卷身份 | 根盘符须经 `GetFinalPathNameByHandleW(VOLUME_NAME_GUID)` 解析为精确的 `\\?\Volume{GUID}\`（拒绝 SUBST 等映射到目录的盘符）；根卷序列号（`FileIdInfo`）记入句柄，每个相对打开 / 创建的对象都核对同卷 | 纯函数 `is_volume_guid_root` 单测（合法 / 大写 / SUBST 形态 / 缺斜杠 / 短 GUID / 错误分隔 / 非十六进制 / UNC / 盘符 / 空）；真实根目录打开通过即证明本机 C: 判定为卷根。**未用真实 SUBST 盘符测试**（会修改用户登录会话的盘符映射，未执行） |
| 累计预算 | 公共常量 `max_output_depth = 128`（自盘符根起算）、`max_output_path_units = 32000`（等效 DOS 路径 UTF-16 单元）；`admit_output_component` 无溢出检查，覆盖 open_existing / create_child / write_new_file（临时名 45 单元一并计入） | 真实目录链：从测试根逐级创建直到失败，断言恰好停在 128 层且错误为 resource_limit，最深层仍可写文件，之后逐层 discard 清理；路径单元上限在真实树中不可达（128 层最多约 30556 单元），以纯函数边界单测覆盖（恰好等于、超 1、已满、两种溢出） |
| 共享模式 | 固定目录改为 `FILE_SHARE_READ \| FILE_SHARE_WRITE`，仍不给 `FILE_SHARE_DELETE` | 其他句柄已以 FILE_ADD_FILE 打开输出目录时仍可固定；固定期间外部程序可把文件移入；固定目录仍不可被重命名（原有断言） |
| 故障注入 | 内部头 `core/filesystem/safe_output_internal.hpp` 的线程局部 `ScopedWriteFaults`：短写、指定偏移写失败（ERROR_DISK_FULL）、每次写后回调、flush 失败、读回损坏、提交失败、显式清理失败 | 200000 字节数据：每次最多 7 字节的短写成功且逐字节一致；70000 字节处写失败、64 KiB 后中途取消、flush 失败、读回损坏（integrity_error）、提交失败，均无最终文件、无临时残留；显式清理失败时错误含临时文件名与“retried at handle close”且关闭句柄后无残留；故障设置不跨线程泄漏 |
| Fuzz | `fuzz/header_fuzz.cpp`（原始 + 修复魔数 / CRC 两路解码、接受即编码往返逐字节相同、块布局性质检查）、`fuzz/path_fuzz.cpp`（单路径组件性质、按行清单冲突检查）；`fuzz/replay_main.cpp` 让普通构建以 CTest 重放种子语料；`msvc-fuzz` preset 构建 libFuzzer 目标 | 见下 |

### Fuzz 实际运行

- 工具：MSVC 19.51（VC Tools 14.51.36231）`/fsanitize=fuzzer /fsanitize=address`，库以 inline-8bit-counters / edge / trace-cmp / trace-div 覆盖插桩。
- 命令（PATH 前置 `VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64` 以加载 ASan DLL）：

```powershell
cmake --preset msvc-fuzz
cmake --build --preset msvc-fuzz --parallel
build/msvc-fuzz/fuzz/Release/seedarc_fuzz_<t>.exe build/msvc-fuzz/work/<t> fuzz/corpus/<t> -max_total_time=150 -max_len=4096 -timeout=10 -rss_limit_mb=2048 -seed=20261111 -print_final_stats=1 -artifact_prefix=build/msvc-fuzz/artifacts/<t>/
```

| 目标 | 时长 | 执行数 | exec/s | 新增单元 | 结束语料 | 峰值 RSS | 崩溃 / artifact |
| --- | --- | --- | --- | --- | --- | --- | --- |
| header | 151 s | 3065144 | 20298 | 299 | 92 / 28 KB，ft 188→851 | 556 MB | 0 |
| path | 151 s | 576342 | 3816 | 2307 | 558 / 69 KB，ft 828→3878 | 520 MB | 0 |

- 种子语料 `fuzz/corpus/`：合法空归档头、单 Store 块头 + 描述符尾、7 字节截断头，及 7 个路径样例。已用临时程序核实前两者被 `decode_header` / `validate_block_layout` 接受、截断头被拒绝；种子中 CRC32C 实现对 `123456789` 得 `0xe3069283`，与 ADR 一致。
- 日志 `build/msvc-fuzz/<t>-run.log`，生成语料在 `build/msvc-fuzz/work/`（不入库）。这是短时运行，**不是持续 Fuzz**；索引 / 块解码入口尚不存在，未覆盖。

## 真实证据（第三轮实际运行）

| 配置 | 构建 | 警告 / 错误行 | CTest |
| --- | --- | --- | --- |
| msvc-vs2026-debug / release | 通过 | 0 | 18/18 / 18/18 |
| gcc-ucrt64-debug / release | 通过 | 0 | 18/18 / 18/18 |
| clang-ucrt64-debug / release | 通过 | 0 | 18/18 / 18/18 |
| msvc-asan（Release，/fsanitize=address） | 通过 | 0 | 18/18，无 ASan 报告 |

- 新增 CTest：`fuzz.replay.header`、`fuzz.replay.path`（种子语料重放，不是 Fuzz）。
- `clang-format --dry-run --Werror`（core / cli / tests / fuzz 全部 .cpp / .hpp）通过；`clang-tidy --config-file=.clang-tidy -p build/clang-ucrt64-debug` 覆盖 13 个 .cpp，退出 0、无诊断。
- 每轮测试后 `.sandbox-test/` 为空。
- CLion MCP：超时根因已定位（见下节），部分文件已取得真实诊断。

## CLion 诊断（第四轮）

- **超时根因**：Nova 后端只分析可见编辑器；MCP 对未打开文件建立隐藏编辑器，永不完成。CLion 窗口失去焦点后，新切换的编辑器也拿不到结果。处理方法写入 [开发说明](development.md#clion-mcp-检查)。证据：同一会话中非 C++ 文件立即返回；`open_file_in_editor` 后、窗口有焦点时 C++ 文件返回诊断；09:16:42 `Window focus lost` 之后的每次查询都超时，即便后端已 `UP_TO_DATE`。
- **由此发现的真实缺陷（ERROR，已修复）**：Clangd 报 `constexpr auto file_rename_information = static_cast<FILE_INFORMATION_CLASS>(10)` 不是常量表达式。Windows SDK `winternl.h` 的该枚举只有 `FileDirectoryInformation = 1`，值域为 [0, 1]，把 10 转换过去是越界的未定义行为；MSVC 不报，命令行 GCC / Clang 用 MinGW 头（枚举完整）也不报。修复：MSVC SDK 下本地声明 `NtSetInformationFile` 的信息类参数为 `ULONG`；MinGW 下直接用它自带的 `FileRenameInformation`（`static_assert == 10`）。修复后 MSVC / GCC / Clang Debug 0 警告、18/18，CLion 对 safe_output.cpp 的 errorsOnly 查询为空。
- **已取得诊断的文件（全部 errorsOnly=false，CLion 在前台、逐个 open_file_in_editor 后查询）**：core/filesystem/safe_output.cpp、safe_output_internal.hpp、archive_path.cpp，core/archive.cpp、checked_arithmetic.cpp、version.cpp，公共头 safe_output.hpp / archive.hpp / archive_path.hpp，cli/main.cpp，tests/safe_output_tests.cpp / archive_tests.cpp / path_tests.cpp / core_tests.cpp，fuzz/header_fuzz.cpp / path_fuzz.cpp / replay_main.cpp。除上面已修复的那一处外**没有 ERROR**；version.cpp 与三个公共头无任何提示。其余只有 WARN / INFO：
  - 有意保留：`<cstring>`、`<winioctl.h>`、`<cstddef>` 被判为“可能未使用”，实际分别用于 `std::memcpy`、`FSCTL_*` / `IO_REPARSE_TAG_*`、`std::size_t`，按“用到什么就包含什么”保留；`UNICODE_STRING` / `OBJECT_ATTRIBUTES` 的 `{}` 零初始化是防御性写法；`discard_empty` 会修改共享状态，不设为 const。
  - 风格建议（形参可设为 const、可用 constexpr / `views::keys` / 结构化绑定、指定初始化器、冗余括号 / static_cast、`stop_token` 按值传递、拼写）：不影响正确性，本轮不做无关重构。

当前没有首个 Git commit，改动均在工作区。没有压缩或性能实验，也未生成研究基准数据。

## 剩余事项与验收建议

1. CLion 复核已完成（见上节）。
2. 真实 SUBST / 重定向盘符的拒绝路径只有纯函数证据；如需真机证据，需用户同意临时创建并删除一个 SUBST 盘符。
3. Fuzz 为 150 s 短跑；持续 Fuzz 与语料管理随 M2 索引解析入口一并建立。
4. 已知设计边界（不阻塞 M1）：故障注入钩子随库编译但仅测试安装；创建的子目录在句柄关闭时不会自动删除（整树回滚属 M2）。
5. M1 门槛（往返、越界 / 溢出防护、资源上限、路径安全、失败清理、动态检查、IDE 诊断）均已有运行证据，待用户验收；2、3 为已知限制。M2 才开始完整索引、输入枚举与流式 Store 归档。

原型测试临时文件位于项目 `.sandbox-test/`；测试只清理自建树中的已知文件和空目录，不使用递归删除或跟随 reparse。
