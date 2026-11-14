# Linux L1 验证记录（DEV-014）

日期：2026-11-13。执行：DEV-014 子会话（由协调会话委派）。分支：`linux-l1`（WSL 克隆 `~/SeedArc`，基于 `9284eae`；本地提交，未推送、未合并）。构建说明见[开发说明第 7 节](development.md#7-wsl--linuxl1--l2--l3)，L2 / L3 草案见 [POSIX 文件系统草案](posix-filesystem-design.md)。

## 1. 范围与结论

L1 = Linux 上配置、编译核心库与 codec，所有不碰文件系统的测试实际运行，加 ASan + UBSan 与短时 libFuzzer。

| 完成标准 | 结果 |
| --- | --- |
| GCC / Clang Debug / Release 0 警告 | 通过：4 套构建 + ASan/UBSan + fuzz 构建，`-Wall -Wextra -Wpedantic`，`warning:` 计数均为 0 |
| 可运行测试全部通过 | 通过：每套 27/27；11 个 Windows-only 测试注册为 DISABLED（见第 4 节） |
| ASan + UBSan 无报告 | 通过：27/27，`-fno-sanitize-recover=undefined`，已确认插桩生效（第 5 节） |
| Fuzz 短跑无崩溃 | 通过：header / path / archive 各 180 s，0 崩溃、0 artifact（第 6 节） |
| 同一输入 Linux / Windows 内存编码哈希比较 | 一次观察：21 个文件 SHA-256 全部一致（第 7 节）。**不构成跨平台保证** |
| Windows 不受影响 | MSVC Release 36/36（在独立临时克隆中构建，未触碰用户 Windows 检出） |

## 2. 环境（实际检查）

WSL 发行版 `Ubuntu-26.04`，16 核；GCC 15.2.0、Clang 21.1.8（libstdc++）、CMake 4.2.3、Ninja 1.13.2、glibc 2.43。vcpkg：`~/vcpkg`（microsoft/vcpkg `fe8e78fa`，工具 2026-10-29），manifest 模式按 `vcpkg-configuration.json` 的 git registry baseline `9eec002d…` 解析，triplet `x64-linux`：**zstd 1.5.7、liblzma 5.8.4**（与 Windows `x64-windows-static-md` 的 vcpkg status 版本一致）。未使用 apt 的 libzstd / liblzma。zip / unzip / pkg-config 在本轮开始时已安装，无需用户补装。clang-format 未在 WSL 安装（可选工具，未阻塞；格式检查用 Windows UCRT64 clang-format 22.1.8 对同一提交执行，见第 8 节）。

## 3. 代码改动

| 文件 | 改动 |
| --- | --- |
| `CMakePresets.json` | 新增隐藏 `vcpkg-linux`（Ninja、`x64-linux`、仅 Linux 主机）及 `gcc-linux-debug/release`、`clang-linux-debug/release`、`clang-linux-asan-ubsan`、`clang-linux-fuzz`；对应 build / test presets 通过隐藏父 preset 限定 Linux。Windows presets 未改，Windows 上 `--list-presets` 不显示 Linux 项 |
| `core/filesystem/archive_path.cpp` | 抽出 `decode_path`；非 Windows 实现**路径 profile 的 ASCII 子集**（见下）；Windows 分支逻辑不变 |
| `tests/CMakeLists.txt` | 非 Windows：`codec.boundary`（`codec_tests --boundary-only`）、`cli.posix_unsupported`；Windows-only 测试注册为 DISABLED 并附原因 |
| `tests/codec_tests.cpp` | 新增 `--boundary-only` 模式（任何平台，只跑内存中的 codec 边界测试）；归档部分保留 Windows 专用 |
| `tests/path_tests.cpp`、`tests/seed_path_override_tests.cpp` | 非 ASCII 期望按平台区分；新增畸形 UTF-8 必须为 `unsafe_path` 的检查（两平台都断言） |
| `tests/check_cli.cmake` | `posix_unsupported`：verify 可用；compress / extract 退出码 5、提示 unsupported，且不留输出 |
| `fuzz/CMakeLists.txt`、根 `CMakeLists.txt` | Clang libFuzzer 分支（`-fsanitize=fuzzer[-no-link],address,undefined -fno-sanitize-recover=undefined`） |

**关键决定：非 Windows 的路径校验只实现 ASCII 子集**（B 级，未改外部契约）。原实现在非 Windows 上让 `validate_archive_path` / `validate_manifest_paths` 一律返回 `unsupported_feature`，导致索引解析、verify、seed 路径覆盖在 Linux 上无法运行。现在：

- 规则与 Windows profile 相同（禁用字符、设备名、点段、尾随点 / 空格、长度 / 段数限额、大小写冲突、文件被当作目录）。
- 大小写冲突按 ASCII a–z / A–Z 判定；对纯 ASCII 字符串，这与 `CompareStringOrdinal(..., TRUE)` 等价。
- 畸形 UTF-8（过长编码、代理项、> U+10FFFF、截断、孤立续字节、非法首字节）返回 `unsafe_path`，与 Windows 相同。
- **合法的非 ASCII 路径一律 `unsupported_feature`**：NFC 与非 ASCII 大小写折叠需要与 Windows 一致的 Unicode 表，L1 不猜测。结果是 Linux 只接受 Windows 也接受的路径（严格子集），不放宽任何检查。
- 已知差异：同时含非 ASCII 字节和其他违规（如 `a\b` + `é`）的路径，Windows 报 `unsafe_path`，Linux 可能报 `unsupported_feature`；两者都拒绝。

`InputTree::open`（L2）与 `SafeOutputDirectory`（L3）的非 Windows 分支**未改**，仍返回 `unsupported_feature`。

## 4. 测试清单（每套 Linux 配置）

实际运行 27 个：`core.boundaries`、`seed.candidate_plans`、`seed.file_rank_overrides`、`seed.path_rank_overrides`、`archive.m3_compatibility`（读取 M3 夹具并 `verify_archive_bytes`）、`archive.boundaries`、`path.boundaries`、`index.boundaries`、`codec.boundary`、`cli.{help, short_help, help_command, version, missing, unknown, extra, search, benchmark, compress_usage, extract_usage, verify_usage, verify_missing, codec_usage, posix_unsupported}`、`fuzz.replay.{header, path, archive}`。

DISABLED（CTest 显示 `Not Run (Disabled)`，标签 `windows-only`）：

| 测试 | 原因 |
| --- | --- |
| `seed.archive_integration` | 压缩真实目录树（输入枚举，L2） |
| `seed.adaptive_file` | 读取真实输入文件（输入枚举，L2） |
| `filesystem.safe_output` | NT 安全输出与故障注入（L3） |
| `archive.store_round_trip`、`archive.store_acceptance` | 磁盘上 compress + extract（L2 + L3） |
| `codec.round_trip` | `codec_tests` 的归档部分（L2 + L3）；其内存边界部分在 `codec.boundary` 中运行 |
| `archive.store_bounded_memory`、`codec.zstd_bounded_memory`、`codec.lzma2_bounded_memory` | 磁盘往返 + Windows psapi 工作集测量（L2 + L3） |
| `cli.round_trip`、`cli.codec_round_trip` | CLI 磁盘往返（L2 + L3） |

结果（`ctest --preset <name>`，最终提交上重跑）：

| preset | 构建警告 | 测试 |
| --- | --- | --- |
| `gcc-linux-debug` | 0 | 27/27 通过，11 DISABLED |
| `gcc-linux-release` | 0 | 27/27 通过，11 DISABLED |
| `clang-linux-debug` | 0 | 27/27 通过，11 DISABLED |
| `clang-linux-release` | 0 | 27/27 通过，11 DISABLED |
| `clang-linux-asan-ubsan` | 0 | 27/27 通过，11 DISABLED |
| `clang-linux-fuzz`（只构建 3 个 fuzz 目标） | 0 | — |
| Windows `msvc-vs2026-release`（临时克隆，提交 `7cd831f`） | 0 条 C++ 警告（仅 23 条 MSB8029：临时克隆位于 Temp 目录下的 MSBuild 提示） | 36/36 通过 |

## 5. ASan + UBSan

- preset：`-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`（编译与链接），Debug；运行时 `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1`、`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`。
- 插桩核对：`seedarc_index_tests`、`seedarc_codec_tests`、`seedarc_fuzz_replay_archive` 均含 `__asan_report_load*` 与 `__ubsan_handle_*` 符号；`compile_commands.json` 中 30 条编译命令带 `-fno-sanitize-recover=undefined`。阳性对照：同一工具链编译的故意堆越界程序被 ASan 报 `heap-buffer-overflow` 并以非零退出。
- 结果：27/27 通过，无 ASan / UBSan / LeakSanitizer 报告。
- 限制：vcpkg 的 zstd / liblzma 未插桩（按 triplet 默认参数构建）；只覆盖 Linux 可运行的测试，磁盘往返路径未在 Linux 上被 sanitizer 覆盖（仍以 Windows MSVC ASan 记录为准）。

## 6. libFuzzer 短跑

构建 `clang-linux-fuzz`（RelWithDebInfo，库 `-fsanitize=fuzzer-no-link,address,undefined`，目标 `-fsanitize=fuzzer,address,undefined`，`-fno-sanitize-recover=undefined`）。三个目标并行，各 `-max_total_time=180`；新输入写到 `~/fuzz-work/20261113T161623Z/<target>-corpus`，仓库 `fuzz/corpus/<target>` 只读，未改动、未入库。

| 目标 | 执行次数 | exec/s | cov / ft | 语料（结束） | 峰值 RSS | 崩溃 / artifact |
| --- | --- | --- | --- | --- | --- | --- |
| header | 14,005,897 | ≈77k | 248 / 483 | 114 | 499 MB | 0 / 0 |
| path | 1,715,919 | ≈9.5k | 368 / 2437 | 584 | 529 MB | 0 / 0 |
| archive | 7,873,273 | ≈43k | 898 / 1979 | 152 | 470 MB | 0 / 0 |

限制：短跑，不是持续 fuzz；Linux 上非 ASCII 路径在 `decode_path` 处即被拒绝，path / archive 目标对非 ASCII 分支的覆盖低于 Windows。

## 7. Linux / Windows 内存编码哈希（一次观察）

探针（不参与构建，源码见[附录](#附录codec_hash_probecpp)）经 `core/codec/codec.hpp` 直接调用 `Encoder` / `Decoder`：3 个确定性输入 × {zstd 1 / 3 / 19, LZMA2 0 / 6 / 9}，每个结果先用本库解码并与原文比较，再写出文件后外部计算 SHA-256。输入：`text-1m`（1 MiB 重复文本变体）、`noise16-1m`（SplitMix64，16 符号）、`noise256-256k`（256 KiB 高熵，预期无法压缩，编码返回 0 即 Store 回退）。

构建：Linux GCC 15.2 Release / Clang 21.1 Release（链接各自 preset 的 `liblibseedarc.a` 与 vcpkg 静态库）；Windows MSVC 19.51 `/O2 /MD`（链接临时克隆 `msvc-vs2026-release` 的 `libseedarc.lib`、vcpkg `x64-windows-static-md`）。

结果：三方全部 21 个文件（3 输入 + 18 编码流）SHA-256 一致；全部往返通过。部分哈希：

| 文件 | 长度 | SHA-256 |
| --- | --- | --- |
| `input-text-1m` | 1,048,576 | `4b488a3263f284bd078d4361be5da959c634f6c3e0d52561fd8a091d7f7481e7` |
| `enc-text-1m-zstd-3` | 2,164 | `269f6e633670bdb5ac23292302dd8d293966f094deeb85f156971391a1396f43` |
| `enc-text-1m-zstd-19` | 168 | `dcd672eafe40189941e9c3d8573a0f24079c096c88bac033ba51e164f635f11c` |
| `enc-text-1m-lzma2-6` | 1,950 | `51bd39706cab69e02e17b09cf5efff3e6c624b95083172ee14f2883e1f69c9a9` |
| `input-noise16-1m` | 1,048,576 | `5d8b11a44c7cd7b1d51b068b64fd788d3a8d2af0599346d56cc4271674b90939` |
| `enc-noise16-1m-zstd-3` | 541,419 | `c289cbeeeaa61491d9302f09bac9151a21d378c6ac66dd70c98f99f53e5720f3` |
| `enc-noise16-1m-lzma2-6` | 545,185 | `5a5441e29da00adfe4303d1dc55144f99ed4434567fd45c19ab3ea4dd101e05f` |

LZMA2 预设 6 与 9 在 1 MiB 块上输出相同（字典被截到块大小，其余参数相同）。**解读限制**：这只是同一 baseline、单线程、三种输入的一次观察；不覆盖完整归档字节（Linux 不能 compress）、其他 CPU / 编译选项 / glibc，也不是 ADR-0003 所说的跨环境逐字节保证。

## 8. 格式与诊断

- clang-format：WSL 未安装；用 Windows UCRT64 clang-format 22.1.8 对临时克隆中同一批改动的 `.cpp` 执行 `--dry-run --Werror`，修复 1 处（`tests/path_tests.cpp` 初始化列表换行），复查通过。
- CLion MCP：本轮未运行（服务连接失败，且 CLion 打开的是 Windows 检出，不是 WSL 克隆）。这是 Linux 端口阶段，按 AGENTS §7.5 需在阶段验收前由协调者补查 `core/filesystem/archive_path.cpp`、`tests/codec_tests.cpp`、`tests/path_tests.cpp`、`tests/seed_path_override_tests.cpp`（errorsOnly=false）。
- clang-tidy：未运行。

## 9. 未运行 / 未覆盖

- Windows 只验证了 `msvc-vs2026-release`；MSVC Debug、MSVC ASan、`msvc-fuzz`、UCRT64 GCC / Clang 未在本提交上重跑（Windows 分支逻辑未改，但 `archive_path.cpp` 有重构，建议合并前补跑 MSVC ASan 与 UCRT64）。
- L2 / L3、磁盘往返、bounded-memory 测试在 Linux 上未运行（DISABLED）。
- 远端服务器（Ubuntu 24.04，glibc 2.39）未构建、未测试。
- 非 ASCII 路径的 Linux 支持未实现。

## 附录：codec_hash_probe.cpp

Linux：`g++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Icore -Icore/include codec_hash_probe.cpp build/gcc-linux-release/core/liblibseedarc.a build/gcc-linux-release/vcpkg_installed/x64-linux/lib/libzstd.a build/gcc-linux-release/vcpkg_installed/x64-linux/lib/liblzma.a`（Clang 同理）；Windows（vcvars64）：`cl /std:c++latest /EHsc /O2 /MD /W4 /permissive- /Zc:__cplusplus /utf-8 /Icore /Icore\include codec_hash_probe.cpp /link libseedarc.lib zstd.lib lzma.lib ntdll.lib bcrypt.lib normaliz.lib advapi32.lib`。运行：`probe <空目录>`，再对目录中 `*.bin` 计算 SHA-256。

```cpp
// Linux L1 observation probe (not part of the build): encodes fixed in-memory inputs through the
// SeedArc codec boundary and writes each input / encoded stream to <out>/ for external SHA-256.
// Usage: codec_hash_probe <existing empty directory>
#include "codec/codec.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace seedarc::experimental;

namespace {
std::vector<std::byte> noise(std::size_t size, std::uint64_t seed, unsigned alphabet) {
    std::vector<std::byte> bytes(size);
    for (auto &value : bytes) { // SplitMix64, as in tests/codec_tests.cpp
        seed += 0x9e3779b97f4a7c15ULL;
        auto z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        value = static_cast<std::byte>(z % alphabet);
    }
    return bytes;
}

std::vector<std::byte> text(std::size_t size) {
    constexpr std::string_view line = "SeedArc compresses fixed chunks; every block is checked.\n";
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(line[(i * 7 + i / 977) % line.size()]);
    }
    return bytes;
}

bool save(const std::string &path, const std::vector<std::byte> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const std::string dir = argv[1];
    std::printf("%s\n", codec::library_versions().c_str());
    const std::vector<std::pair<std::string, std::vector<std::byte>>> inputs{
        {"text-1m", text(1 << 20)}, {"noise16-1m", noise(1 << 20, 42, 16)},
        {"noise256-256k", noise(256 << 10, 7, 256)}};
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> configs{
        {codec::zstd_id, 1}, {codec::zstd_id, 3}, {codec::zstd_id, 19},
        {codec::lzma2_id, 0}, {codec::lzma2_id, 6}, {codec::lzma2_id, 9}};
    int failures = 0;
    for (const auto &[name, input] : inputs) {
        failures += save(dir + "/input-" + name + ".bin", input) ? 0 : 1;
        for (const auto &[id, level] : configs) {
            auto encoder = codec::Encoder::create(id, level);
            if (!encoder) {
                ++failures;
                continue;
            }
            std::vector<std::byte> out(input.size() - 1);
            const auto size = encoder->encode(input, out);
            if (!size) {
                ++failures;
                continue;
            }
            out.resize(*size);
            const std::string label =
                (id == codec::zstd_id ? "zstd" : "lzma2") + std::string{"-"} + std::to_string(level);
            if (*size != 0) {
                std::vector<std::byte> back(input.size());
                codec::Decoder decoder;
                if (!decoder.decode(id, out, back) || back != input) {
                    std::printf("ROUND TRIP FAILED %s %s\n", name.c_str(), label.c_str());
                    ++failures;
                }
            }
            std::printf("%-14s %-8s %8zu\n", name.c_str(), label.c_str(), *size);
            failures += save(dir + "/enc-" + name + "-" + label + ".bin", out) ? 0 : 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
```
