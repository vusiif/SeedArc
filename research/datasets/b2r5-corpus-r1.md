# b2r5-corpus-r1：本机独立语料（开发集 / 测试集）

创建：2026-11-12，DEV-012，[`research/scripts/b2_corpus.py`](../scripts/b2_corpus.py)。机器可读清单：[b2r5-corpus-r1.json](b2r5-corpus-r1.json)；逐文件清单（相对路径、字节、SHA256）：[b2r5-corpus-r1/](b2r5-corpus-r1/)`<split>-<pool>.tsv`。内容本身在被忽略的 `research/local/b2r5-corpus-r1/`，**不提交、不分发**。

## 来源与许可

**没有下载任何外部数据**，全部取自本机已安装的软件和系统文件。清单只记录经过脱敏的来源描述，不写绝对路径，因为路径里可能含用户名。

| 数据池 | 类型 | 来源 | 许可 / 条件 |
| --- | --- | --- | --- |
| source-headers | 源码 / 文本 | MSYS2 ucrt64/include | 各包许可；mingw-w64 头文件为 ZPL-2.1 / 公有领域；仅本地评估 |
| config-cmake | 配置 / 文本 | CMake 4.4.3 share/ | BSD-3-Clause；仅本地评估 |
| logs-windows | 日志 | 本机 Windows\Logs 中的 .log / .txt | 机器生成，可能含机器标识；**只在本地使用，不提交** |
| structured-inf | 结构化数据 | Windows\INF 中的 .inf | Microsoft / 硬件厂商；不可再分发 |
| binary-native | 原生二进制 | MSYS2 ucrt64/bin 中的 DLL / EXE | 各包许可；仅本地评估 |
| compressed-packages | 已压缩（zstd 包） | MSYS2 pacman 缓存中的 *.pkg.tar.zst | 各包许可；仅本地评估 |
| image-jpeg | 已压缩（图片） | Windows\Web 中的 jpg / png | Microsoft；不可再分发 |
| audio-pcm | 音频（**未压缩** PCM WAV） | Windows\Media 中的 wav | Microsoft；不可再分发 |
| many-small-terminfo | 大量小文件 | MSYS2 ncurses terminfo | MIT-X11 风格；仅本地评估 |
| duplicate-licenses | 跨文件重复 | MSYS2 usr/ 与 ucrt64/ 下的 share/licenses | 许可文本本身；仅本地评估 |

图片和已压缩包可以覆盖“已压缩”这一类；本机没有现成的视频文件，所以这一类**没有视频样本**。WAV 是未压缩音频，单独列为一类，不当作已压缩数据。

## 划分与选择（在任何压缩运行之前确定）

- **划分**：`sha256("b2r5-corpus-r1:<pool>:<相对路径>")` 首字节为偶数归入 dev，奇数归入 test。划分结果只取决于路径，与内容、压缩结果都无关。
- **选择**：每个划分内按相对路径排序依次选入，直到达到上限（多数数据池 12 MiB，binary-native 和 compressed-packages 为 16 MiB；每个划分最多 2000 个文件）。单个文件超过 4 MiB 的跳过（image-jpeg 放宽到 8 MiB）；跳过的数量逐池记在清单里。
- **用途**：test 只用于独立评估，**绝不参与任何阈值或参数调整**；dev 留给以后的 r2 调参，本轮没有读取。

## 规模（test 划分）

| 数据池 | 文件数 | 字节 | 64 KiB 块数 | 不同内容数 |
| --- | ---: | ---: | ---: | ---: |
| source-headers | 798 | 12,582,909 | 865 | 787 |
| config-cmake | 2,000 | 4,626,840 | 2,007 | 1,972 |
| logs-windows | 52 | 7,158,056 | 145 | 52 |
| structured-inf | 241 | 12,582,874 | 373 | 240 |
| binary-native | 30 | 16,774,829 | 272 | 27 |
| compressed-packages | 32 | 16,764,602 | 272 | 32 |
| image-jpeg | 11 | 6,180,159 | 100 | 11 |
| audio-pcm | 38 | 10,500,266 | 180 | 38 |
| many-small-terminfo | 1,477 | 1,689,703 | 1,477 | 1,132 |
| duplicate-licenses | 52 | 625,576 | 52 | 39 |

dev 划分的规模见 JSON 清单。所有 20 个数据集都已用 zstd3 确认可以正常归档（路径合法、不超限额），这一步只检查可行性，不看压缩率。

## 恢复方式

在同一台机器、同一批已安装软件的前提下，重新运行 `python research/scripts/b2_corpus.py --corpus-id <新 ID>` 即可重建。重建后用逐文件 TSV 里的 SHA256 核对；系统日志、pacman 缓存等会随时间变化，核对不一致时必须记为新版本语料。这份语料不适合在其他机器上逐字节复现，其他机器只能按同样的规则重建出同类语料。

## 已知缺陷（2026-11-12 登记，DEV-013 复核）

r1 只按**路径**哈希划分，相同内容可能同时落在两侧。复核结果：test 中有 **614 个文件**（427 种内容）的 SHA256 也出现在 dev，其中 many-small-terminfo 540、config-cmake 37、duplicate-licenses 23、source-headers 13、binary-native 1。DEV-012 没有用 dev 调参，结果按原记录保留、不改写；但这几个数据池不能算作干净的留出评估。修正版本见 [b2r5-corpus-r2](b2r5-corpus-r2.md)。
