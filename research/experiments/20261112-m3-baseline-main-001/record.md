# 20261112-m3-baseline-main-001：固定算法合成基线

状态：已完成（本地探索；未独立复现）。负责人：主 Agent；关联 DEV-005。

## 问题与预设方法

为 M4 种子候选比较建立可复现的 M3 固定算法参照；不检验独创性，不检验 Seed 收益。方法来自 seed-design-study 的固定算法基线，3 个合成输入、Store / Zstd-3 / LZMA2-6、每组 3 次；各重复轮转 codec 顺序，无预热，OS 缓存未控制。所有输入仅 256 KiB 或 512 KiB，小于默认 1 MiB 分块，不覆盖多块、媒体或真实用户目录。大小、端到端耗时为主要指标；内存未测量，写为 null。

## 代码、环境与复现

使用 `77764b9` 已验收 M3 的现有 MSVC Release CLI，复制后运行，未因本轮种子改动重新构建。SHA256 为 `3a3b0fcc414d25288b8763d76233931fca5d8f709407b485d9a254ff1f4b102c`。CLI 依赖版本输出在 config.json。本轮工作区有设计文档 / 新实验脚本改动（状态快照在 config），不参与此二进制；运行脚本完整副本 m3_baseline.py 与哈希随实验保存。

构建来源：既有 M3 MSVC 19.51 / VS 2026 Release，`/O2 /Ob2 /DNDEBUG`，vcpkg 固定 zstd 1.5.7 / liblzma 5.8.4、`x64-windows-static-md`，核心单线程。精确环境、时间（America/Los_Angeles）、二进制哈希、脚本哈希和命令见 config.json。机器 RAM 与峰值内存未测量。编译工具链仅按既有 M3 记录及缓存核对，本次没有重建此基线二进制。

在项目根目录，以正常用户执行（避开已知沙盒 ACL 干扰）：

```powershell
python research/scripts/m3_baseline.py --cli build/msvc-vs2026-release/cli/Release/seedarc.exe --run-id <新的-run-id> --repetitions 3
```

如需从源码复现二进制，先从 `77764b9` 的独立 checkout 按开发说明配置 vcpkg 并构建 `msvc-vs2026-release`，再运行脚本。该脚本不验证调用者提供的二进制确为 Release，使用者须保留对应构建证据。原始数据由脚本生成；datasets.json 记录每个文件大小与 SHA256。重复文本与 SHA-256 计数器字节为项目生成数据，按 GPL-3.0-or-later 使用，无外部数据授权问题。完整 inputs / archives / extracted outputs 与二进制保留在被忽略的 `research/local/20261112-m3-baseline-main-001/`，可按脚本再生成。

## 原始结果与分析

raw.json 保存 27 次完整命令及指标；每次 compress / verify / extract 日志单独保存，全部退出 0，27/27 按文件清单、大小和 SHA256 往返一致。summary.json 由运行脚本从原始结果汇总，保存每组完整归档大小、哈希和压缩耗时中位数。耗时包含进程启动、写前自检、I/O 等，不是纯编码吞吐；三个重复太少且样本很小，不能据此比较算法性能。

| 合成输入 | Store 归档字节 | Zstd-3 归档字节 | LZMA2-6 归档字节 |
| --- | ---: | ---: | ---: |
| 重复文本 | 262332 | 282 | 368 |
| 计数器哈希字节 | 262333 | 262333 | 262333 |
| 混合树（含空文件） | 524641 | 262591 | 262677 |

每组重复归档哈希相同；高熵合成输入三种选项归档相同，符合 Store 回退行为。这只是小样本观察，不推导一般压缩率、速度或 Seed 优劣。尚无 Seed 搜索、无独立评估集、无相关工作检索或统计显著性结论。后续需加入多块、真实且授权明确的数据，以及独立评估划分。

## 固定旧格式样例

保存 zstd / lzma2 文本的第 0 次归档到 tests/fixtures/m3；另用同一二进制压缩空目录得到 96 字节 Store 样例。SHA256 见该目录 manifest.json。预期与生成方法见该目录 README。保存这些样例不表示冻结实验格式；原始实验结果未覆盖。

## 独立验证

尚未由另一执行者独立复现。原始运行与结果核对均由主 Agent 执行。后续固定样例测试的构建与 CTest 证据另见 M4-A 验证记录。
