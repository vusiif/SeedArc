# 跨 CPU / 跨构建逐字节一致性核对

2026-11-26，DEV-031，主Agent（Claude），无委派。状态：执行者报告通过，**未经独立审计**；**只做核对，未改格式或代码**（只修了核对脚本自身的一个缺陷，见下文"发现的问题"）。

## 问题

[AGENTS.md](../../../AGENTS.md) §6 把"跨环境逐字节相同"列为一项单独的承诺。此前只在本机 MSVC / GCC / Clang 之间，以及印度上，用写入端固定小包核对过。Zstd 和 LZMA 会按 CPU 特性挑选不同的代码路径。所以要问：同一个程序、同样的输入、同一个配方，在不同 CPU 上写出的包，是否逐字节相同？换一次构建、换编译器以后呢？

## 方法

- **核对脚本** [cross_cpu_check.py](../../scripts/cross_cpu_check.py)：每台机器用同一个程序、同一份输入，执行一组固定的写操作；每个包先 `verify` 逐字节验证，再记下 SHA256。比较时只看字节，耗时不比。
  - 配方：Zstd 1 / 3 / 9 / 19、LZMA2 0 / 6 / 9（frame-16MiB、g1-r0），外加 Zstd-9 和 LZMA2-6 的 whole 帧 / g2-r1，以及 Zstd-9 的 explicit 表示；
  - 输入：两个合成夹具（`bounded_fixture.py` 的 small/all、`recipe_fixture.py` 的 big），一个 Linux 源码子集，3 个 Silesia 文件；
  - Linux 子集还做了三件事：按路径哈希分成 70 / 30，做一次打包 + 追加 + 整理（Zstd-9、LZMA2-9 各一遍）；对整理后的包做 `verify`；跑 drift-probe 的 3 / 9 两档，只比字节字段。
  - 共 46 个包、2 个探针结果，外加 4 棵输入树的摘要。
- **数据**：由印度任务按固定规则生成。
  - 从 DEV-023 源码树清单里取 `linux/` 下的文件，按 `sha256('dev031-subset:' + 路径)` 排序，累计到 40 MiB 为止：2 601 个文件，41 943 033 字节，逐个按清单核对 SHA；
  - Silesia dev zip（SHA 已核对）里最小的 3 个成员：xml、reymont、sao；
  - 打包为 `xcpu-data.tar.xz`，SHA `153756043c08…`。
- **程序包**：在印度用固定的 GCC 14.2 构建，打成 `xcpu-tool.tar.gz`，SHA `d80c200dd6db…`。
  - `ldd` 显示只依赖系统的 glibc / libgcc_s，C++ 运行库是静态链接的，所以没有另带库文件；
  - 印度那一轮也是从**解开后的包**运行，和其他机器执行的是完全相同的字节。

## 参与的环境

| 环境 | CPU | 程序 | 编译器 / 运行库 |
| --- | --- | --- | --- |
| 印度 vps-in1（参考） | AMD EPYC 7K62（Zen 2），有 AVX2 | 印度构建 | 固定 GCC 14.2.0，静态 libstdc++ |
| vps-us2 | Intel Xeon E312xx（Sandy Bridge），**无 AVX2** | 印度构建 + us2 本机构建 | 本机构建：Ubuntu `g++-14` 14.2.0-4ubuntu2~24.04.1，动态链接系统 libstdc++ |
| vps-hk1 | AMD EPYC 7R13（Zen 3），有 AVX2，1 核 / 961 MB | 印度构建 + us2 本机构建 | 同上 |
| 本机 Windows 10 | AMD Family 25 Model 116 | MSVC Release 构建 | MSVC，vcpkg x64-windows |

所有构建都用 zstd 1.5.7 和 liblzma 5.8.4，vcpkg baseline 都是 `9eec002d`。

## 结果

| 比较（都对比印度参考） | 输入摘要 | 46 个包 | 2 个探针结果 |
| --- | --- | --- | --- |
| us2，印度构建 | 相同 | **全部相同** | 相同 |
| us2，us2 构建 | 相同 | **全部相同** | 相同 |
| hk1，印度构建 | 相同 | **全部相同** | 相同 |
| hk1，us2 构建 | 相同 | **全部相同** | 相同 |
| Windows，MSVC 构建 | 相同（修正脚本后） | **全部相同** | 相同 |

**结论（只限本次覆盖的范围）**：在 codec 版本固定的前提下，写入结果与以下因素无关：
- CPU 厂商、代数，以及有没有 AVX2；
- 是同一个程序还是重新构建的程序（包括静态与动态 libstdc++）；
- 用 GCC 14.2 还是 MSVC；
- 跑在 Linux 还是 Windows 上。

覆盖的内容有：Zstd 1–19 和 LZMA2 0–9 中抽取的 7 个档位、两种分组、seed / explicit 两种表示、追加、整理、drift-probe。

**不能外推的地方**：
- codec 版本变了，输出可能会变。比如换成别的 zstd 版本，压缩结果的字节不保证相同，这正是版本必须固定的原因。
- Clang 构建只参与了测试，没有参与这次的 SHA 比较。
- ARM 等其他架构没有覆盖。
- 输入是 dev 数据，不是 holdout。

## 同时完成的环境验证

| 环境 | 构建 | 结果 |
| --- | --- | --- |
| us2 | GCC 14 Release | 0 警告，CTest 49/49；写入端固定小包的 SHA 与印度参考一致 |
| us2 | Clang 20 ASan + UBSan（项目预设的参数） | 0 警告，CTest 49/49，0 个 sanitizer 报告 |
| 印度 | 固定 GCC 14.2 Release | CTest 49/49，写入端固定小包的 SHA 与参考一致 |

us2 上的 ASan 起初两次都没跑起来，都是环境问题，没有改代码：
1. CMake 3.28 用 Clang 构建 C++23 时要扫描模块依赖，需要 `clang-scan-deps`，原环境里没有，补装 `clang-tools-18`；
2. Clang 18 报告的 `__cpp_concepts` 低于 libstdc++ 14 的 `<expected>` 所要求的值，所以 `std::expected` / `std::unexpected` 不可用。这是 Clang 18 的已知问题，Clang 19 起已修复，因此改用 Ubuntu 24.04 提供的最新版 Clang 20（WSL 上用的是 Clang 21）。

失败那两次的日志分别保存在 `remote-vps-us2-native/`（asan-*）和 `remote-vps-us2-asan-clang18-failed/`。

## 发现的问题

**核对脚本自身的缺陷（已修）**：`tree_digest` 原来用 `sorted(Path.rglob())` 决定哈希顺序。`Path` 是按路径分量排序的，而且在 Windows 上不区分大小写，在 Linux 上区分。于是同一棵树在 Windows 上算出了不同的摘要（`local-win-msvc/xcpu-win-msvc-digest-bug.json`），可那一次 46 个包其实全都相同。

排查过程：
1. 先确认这批数据里没有只差大小写的文件名，排除了 Windows 文件系统把两个文件合并成一个的可能；
2. 再按 Linux 的排序规则重算，摘要与印度一致。

修正方法：改为显式按 `PurePosixPath(rel).parts` 排序，也就是区分大小写的分量顺序。这正是 Linux 上原本的顺序，所以三台 Linux 机器已有的摘要依然有效；修正后的 Windows 重跑也完全一致。教训：**跨平台核对代码里，凡是涉及排序的地方都要写明比较规则**。归档写入端本身没有这个问题，它按 UTF-8 字节序排列路径，46 个包全部相同也证明了这一点。

## 小内存摸底（hk1）

hk1 跑核对时，每 5 秒采样一次内存（[memory-trace.txt](remote-vps-hk1-check/memory-trace.txt)）：
- 内存最高用到 820 MiB，总量 961 MiB；
- swap 最多比开始时多出约 43 MiB；
- LZMA2-9 和 Zstd-19 在 42 MB 输入上都跑完了，没有被系统杀掉。

这只是摸底：5 秒的采样间隔会漏掉短时间的峰值。研究工具会把整棵树读进内存，数据再大就会不够用，正式实现必须改成流式读写。

各机器上整套命令的耗时（仅供参考，没钉核，条件也不同）：印度 311 s、us2 361 s、hk1 302 s、Windows 约 5 min。单线程速度三台服务器相近。

## 工具与环境变更（记录在案）

- us2 和 hk1 的任务工具，都由 `research/local/*/make_*_client.py` 从 in-long-task 技能派生，派生前核对技能的远端脚本为 `4118a90c…`。派生副本放在不进 Git 的 `research/local/`，内含服务器地址；原技能没有改动。改动内容：
  - 连接改走 Tailscale，主机公钥严格校验，各用单独的公钥文件；
  - 远端核对主机名；
  - 共享模式资源上限按机器大小调整；
  - 内置 DEV-023 审过的磁盘监控修正；
  - RPC 时限从 90 s 放宽到 600 s。原因是第一次在 us2 上准备任务时，上传 14 MiB 输入超时；当时已确认服务器上没有建出任何东西，那个任务 ID 标为放弃，没有盲目重试。
- us2 的系统改动（用户授权可随意使用）：apt 安装了 build-essential、g++-14、clang-18 / 20 及 clang-tools、cmake、ninja 等；新增 2 GiB `/swapfile`；`~/seedarc-env` 下有 vcpkg（`9eec002d`）和 zstd / liblzma。
- hk1：用户授权可随意使用。本次只运行了任务，没有安装任何软件。

## 复现

```text
python cross_cpu_check.py run <seedarc_append4_study> <含 linux/ 与 silesia/ 的数据目录> <新工作目录> <输出.json>
python cross_cpu_check.py compare <参考.json> <对比.json>
```

印度一侧的入口是 [cross_cpu_india_task.py](../../scripts/cross_cpu_india_task.py)，负责构建、打包程序、生成数据子集，并从解开后的包运行核对。
