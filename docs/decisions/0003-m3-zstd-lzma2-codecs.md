# ADR-0003：M3 Zstandard / LZMA2 块编码与依赖

状态：**M3 实验原型已验收（2026-11-12）**；experimental、未冻结。初稿日期：2026-11-11。

输入：[开发计划 M3](../development-plan.md)、[架构](../architecture.md)、[ADR-0001](0001-experimental-container-and-output.md)、[ADR-0002](0002-m2-store-archive-constraints.md)、[开发说明 §4](../development.md)。

## 1. 依赖来源（vcpkg manifest，全工具链同一 baseline）

| 组件 | 版本 | 许可 | vcpkg port / CMake target |
| --- | --- | --- | --- |
| Zstandard (libzstd) | 1.5.7 | BSD-3-Clause OR GPL-2.0-only（按 BSD-3-Clause 使用） | `zstd` / `zstd::libzstd` |
| liblzma（XZ Utils） | 5.8.4 | 0BSD（仅库；未启用 LGPL 的 `tools` 特性） | `liblzma` / `liblzma::liblzma` |

- `vcpkg.json` 声明依赖并用 `overrides` 钉住上述版本；`vcpkg-configuration.json` 以 git registry 固定 baseline `9eec002df1cd109f8f104cd966a763daf9751489`（microsoft/vcpkg tag `2026.10.16`）。不依赖本机已装包，也不依赖 vcpkg 实例自带的 registry 克隆。
- triplet：MSVC 为 `x64-windows-static-md`（静态库、动态 CRT，与项目 /MD 一致）；GCC / Clang（MSYS2 UCRT64）为社区 triplet `x64-mingw-static`，host triplet 同。MSVC 与 GNU ABI 的库分别构建，不跨 ABI 链接。
- **选择 vcpkg 而非 MSYS2 系统包**：两套 ABI 使用同一 port 版本，否则同一输入在不同工具链下可能得到不同的压缩字节，影响 M4 的跨环境复现。
- liblzma 5.8.4 不是含后门的 5.6.0 / 5.6.1；两者都只作库链接，不引入命令行工具。
- 两个依赖都以 vcpkg manifest 管理，按 [third_party 规范](../../third_party/README.md) 不复制源码入库。许可均与 GPL-3.0-or-later 兼容；发布二进制时须附带其许可文本（属发布阶段工作，未做）。
- `VCPKG_ROOT` 是机器相关路径，presets 通过 `$env{VCPKG_ROOT}` 引用，不硬编码。

## 2. 块编码与索引规则

沿用 ADR-0001 / 0002 的头部与索引布局，不新增字段；`codec_id` / `codec_config_id` 由原来的“仅 0”扩展为：

| codec_id | 名称 | codec_config_id | 存储长度规则 |
| --- | --- | --- | --- |
| 0 | Store | 0 | `stored_size == raw_size` |
| 1 | Zstandard | 压缩级别 1–19 | `0 < stored_size < raw_size` |
| 2 | LZMA2 | liblzma 预设 0–9 | `0 < stored_size < raw_size` |

其他组合一律 `unsupported_feature`；长度不符为 `malformed_archive`。“压缩块必须严格变小”同时为解码缓冲给出上界（≤ 一个分块）。

- **Store 回退**：每个分块独立压缩；压缩结果不严格小于原文时按 Store 写入，该块记录 codec 0 / config 0。块记录长度固定为 48 字节，回退不增加索引开销；索引如实记录每块实际采用的编码。
- **配置只影响编码器**：解码不依赖 `codec_config_id`。Zstandard 帧自带参数；LZMA2 解码字典取 `max(raw_size, 4096)`，因为匹配距离不可能超过已解码长度，无需在归档中记录编码字典。
- **Zstandard 帧**：每块恰好一个常规帧；关闭帧内校验（块已有 CRC32C）、不写字典 ID、必须写内容大小，单线程。每块都显式重设全部参数。解码要求：恰好一帧且无尾随字节、声明的内容大小等于 `raw_size`、窗口不超过覆盖块大小的 2 的幂（按 RFC 8878 自行解析窗口描述符；单次解码以输出缓冲作历史，内存不随声明窗口增长）；拒绝跳帧和未声明大小的帧。
- **LZMA2 流**：原始 LZMA2（`lzma_raw_buffer_encode/decode`，无 .xz 容器、无额外校验），预设字典上限取 `min(预设字典, 块大小)`。解码要求到达结束标记、恰好消耗全部输入并产出恰好 `raw_size` 字节；解码前用 `lzma_raw_decoder_memusage` 检查内存不超过字典加 1 MiB。
- **写出自检**：每个压缩块写入前先用本库解码器还原并与原文逐字节比较，不一致即失败，不写出伪装成功的归档。
- **头部回填**：`data_bytes` 依赖压缩结果。写出端先用上界（原文总长）做全部限额检查，预留 96 字节头部，写完数据与索引后回填；`SafeOutputFile` 提交时逐字节核对回填的头部，其余字节仍与增量 CRC32C 比较。

## 3. API 与 CLI

- `compress_store` 更名为 `compress_archive`；`CompressOptions` 新增 `codec`（`store` / `zstd` / `lzma2`，默认 store）与可选 `level`（zstd 默认 3，LZMA2 默认 6；Store 给 level 即 `invalid_argument`）。
- CLI：`seedarc compress … [--codec store|zstd|lzma2] [--level N]`；参数错误退出码 2。`--version` 额外输出链接的库版本。
- 第三方头文件只出现在 `core/codec/codec.cpp`；`core/codec/codec.hpp` 不含第三方类型或错误码。

## 4. 确定性与限制

- 同一输入、选项与库版本下，归档字节可复现（测试覆盖同一构建内重复压缩）。**跨工具链逐字节一致尚未验证**，属 M4 范围；zstd / liblzma 升级可能改变压缩输出，因此版本钉死并随归档报告。
- 内存（单次功能性观测，64 MiB 低熵输入、1 MiB 分块、MSVC Release）：峰值工作集增长 Store 约 1.4 MiB、zstd-3 约 4.6 MiB、LZMA2-6 约 14.9 MiB；测试断言上界分别为 16 / 16 / 32 MiB。这不是基准。
- 未做：多线程压缩、字典 / 固实压缩、其他算法、可用磁盘空间预检、按 codec 的独立内存预算对象（各项由块大小与上述检查直接约束）。
