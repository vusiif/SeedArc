# 压缩 trailer 的实现与测量（DEV-044）

2026-11-28，主 Agent（Claude），无委派。状态：执行者报告，**未经独立审计**。依据：[ADR-0010](../../../docs/decisions/0010-compressed-trailer.md)，用户 2026-11-28 接受方案 A 并要求实现。

## 实现

**容器**（research/tools/append4_study.cpp）
- 修订 5 的头部新增标志 bit 2（`flag_zstd_trailer`）。
  - 修订 4 仍只接受 bit 0–1；
  - 修订 5 接受 bit 0–2；
  - 其余位一律报 `bad header`。
- 置位时，trailer 存成 `原始长度（8 字节）| 一个 zstd-19 帧`：记录内容大小，不加校验和，不用字典。footer 的 CRC 覆盖整个存储形式。
- 解码分两步：
  1. 先检查 0 < 原始长度 ≤ 256 MiB，通过后才分配内存。256 MiB 和索引读取原有的 trailer 上限相同；fuzz 构建里降到 16 MiB。
  2. 再用 `research::decompress_solid`，要求恰好一个帧、内容大小等于原始长度、没有字典、窗口 ≤ 2^27、帧后没有多余字节。
- 解出的字节交给原来的 `parse_index` 解析，条目、帧表和交叉检查都不变。
- 两个读取路径都经过 `trailer_view`：
  - 完整解析，恢复也走这条路径，会逐个读取前缀里旧的 trailer；
  - 只读索引的提取（bounded_extract.hpp）。
- 写入：`finish_container` 改为返回错误，压缩失败时不写出归档。

**写入策略**
- 新写的修订 5 归档默认压缩 trailer。最前面可以加 `--trailer raw|zstd` 选项。
- recipe-append 和 append 沿用原归档的形式。显式给出相反的形式时，拒绝执行。
- consolidate 默认沿用原归档的形式，给了 `--trailer` 就按给定的形式写。
- 新命令：
  - `trailer-recode ARCHIVE OUT raw|zstd`：只改单代归档的形式，所有记录逐字节保留；改完重新解析，要求得到相同的索引。多代归档里旧的 trailer 沿用当初写入时的形式，所以多代归档只能通过 consolidate 改形式。
  - `trailer-fault`：测试辅助命令，写出 9 种没有写入器会产生的存储形式。
- describe 新增一项 `"trailer":{"form","stored_bytes","raw_bytes"}`。

**测试**（check_solid_recipe.cmake 新增一节）
- 默认形式和 `--trailer zstd` 打出的包逐字节相同；raw 和 zstd 两种形式的载荷相同，verify、describe、只读索引的提取都正确。
- 大小核对：两个包的大小差正好等于两个 trailer 的大小差。
- recode 互转：raw → zstd 的结果和直接打 zstd 包逐字节相同；反方向也一样。
- 追加后形式不变，合并后形式不变；`--trailer zstd consolidate` 的结果和直接打包相同。多代归档上的 recode 被拒绝。
- 拒绝：追加时给出相反的 `--trailer`、拼写错误的选项。
- 9 种故障，每一种都要求 verify、describe、extract 全部拒绝，并且什么都不写出：
  - 原始长度超出上限 / 少 1 / 为 0 / 缺失；
  - 两个帧；
  - 帧后多一个字节；
  - zstd 标志下存原始字节；
  - 清除标志下存 zstd 字节；
  - 头部 bit 3。
- 原有测试的调整：
  - recipe_fixture.py 的 retag / realias 需要直接改原始 trailer，所以它们处理的源包改为用 `--trailer raw` 打包，共 10 处。拒绝用例本身不变。
  - `research.solid_search` 里"Seed 与显式两种表示得分相同"的比较改用 raw 形式。两种表示的 GEN 记录大小相同、内容不同，压缩后会差一两个字节。
- fuzz：
  - 修复 CRC 时，zstd 形式只修 footer 的 CRC，因为头部 CRC 在压缩的数据里面；
  - 新增样本 `r5-trailer-zstd-seed.sadx`（459 B）和 `r5-trailer-zstd-explicit-append.sadx`（1 153 B，两代、显式表示、字典加去重）；
  - 原有的 19 个样本都是 raw 形式，保留下来作为回归用例。

## 验证

- MSVC Release（build/dictstudy-check）：0 警告，CTest 50/50。
- fuzz 语料回放：21 个，退出码 0。
- WSL GCC、ASan+UBSan 和 1 200 s fuzz：见下面"WSL 与 fuzz"一节。
- CLion：未运行（本阶段暂缓，MCP 也连不上）。

## 测量

- **环境**：本机，`--jobs 1`。工具 SHA256 前缀 `4ed1c0de…`。
- **数据**：DEV-040 用的同一批树，加上 npm。未碰留出集。
- **脚本**：仓库外的 `trailermeasure.py`，已抄进 `commands.md`。
- **原始数据**：`local/raw.jsonl`。

**1. 精确节省**：用 `solid/zstd-3/g1-r0/frame-32MiB` 打一个 raw 形式的包，再 recode 成 zstd。两个包的其余字节完全相同，所以大小差就是 trailer 带来的全部差别。

| 树 | raw trailer | zstd 存储（含 8 B 长度） | 整包节省 | DEV-041 估算的 zstd-19 + 8 | recode 用时 |
| --- | ---: | ---: | ---: | ---: | ---: |
| append-r1 dev | 265 757 | 63 425 | 202 332 | 63 425 | 0.26 s |
| cargo | 1 086 359 | 220 326 | 866 033 | 220 364 | 0.38 s |
| rustup | 30 822 | 6 247 | 24 575 | 6 247 | 0.19 s |
| 全部 | 1 289 760 | 244 378 | 1 045 382 | 244 377 | 0.70 s |
| npm | 77 111 | 17 853 | 59 258 | 17 853 | 0.07 s |

- 实测和 DEV-041 用 Python zstd-19 的估算相差 0–38 B。两边的 zstd 库版本不同，差异只在 cargo 上出现。

**2. 最终配方**：`solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup`，按默认用 zstd 形式打包，再用 verify 逐字节比对。

| 树 | DEV-043（raw） | 本次（zstd） | 变化 | 7-Zip 26.03 -mx9 | tar \| xz -9e | xz 256 MiB（DEV-042） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| append-r1 dev | 35 580 642 | **35 378 301** | −0.57% | 36 065 458 | 35 445 856 | — |
| cargo | 19 426 153 | **18 549 109** | −4.51% | 18 931 714 | 19 539 944 | 18 619 240 |
| rustup | 44 311 933 | **44 287 436** | −0.06% | 46 553 189 | 48 782 200 | 44 312 736 |
| 全部 | 65 435 937 | **64 378 752** | −1.62% | 67 016 273 | 69 937 464 | — |
| npm | — | **1 653 715** | — | 1 661 721（7z 23.01） | 1 659 804 | — |

- 本轮测过的所有工具里，这五棵 dev 树都是 SeedArc 最小。
  - 和 7-Zip 26.03 比：cargo −2.0%，rustup −4.9%，全部 −3.9%，append −1.9%；
  - 和 tar|xz -9e 比：cargo −5.1%，全部 −7.9%，npm −0.4%。
- 整体帧的 trailer 比 32 MiB 帧的更好压。例如 cargo 是 209 083 B 对 220 326 B，因为所有条目的帧号都是 0。

**3. 打开索引的代价**："全部"树（16 258 个文件），zstd-3 的 32 MiB 帧，用 extract 提取同一个文件，两种形式交替各跑 5 次：
- 只读索引耗时：raw 50–63 ms，zstd 41–48 ms；
- 读取的索引字节：1 289 880 降到 244 498。
- 解压的开销被少读的字节抵消了，打开索引没有变慢。

## WSL 与 fuzz

- **快照**：工作树快照 `b450107` 和 `dbbaa88`。后者只在前者基础上改了 CMakeLists 里的测试时限。两个临时 ref 用完都已删除。
- **GCC 14 Release**：0 警告，CTest 51/51，用时 406 s。
- **Clang ASan+UBSan**：0 警告。22 项里先过了 21 项。
  - 剩下的 `research.solid_recipe` 两次都在 900 s 的 TIMEOUT 处被截停。那两次都和 fuzz 等任务同时在跑，不是测试本身出错。
  - 另外发现：`ctest --timeout` 盖不过测试自己的 TIMEOUT 属性。
  - 处理：把这个测试的 TIMEOUT 改成 2400 s，research/tools/CMakeLists.txt 里注明了原因。改完单独重跑，**798.9 s 通过，没有 Sanitizer 报告**。
  - 按用户同日定下的规则，墙钟时间超时只当作估计，所以这里把"截停"和"失败"分开报。
- **fuzz**：libFuzzer + ASan，目标 SHA 前缀 `2dc82f23…`，构建 0 警告。
  - 语料：1 132 个，回放退出码 0。
  - 正式运行：1 201 s，2 686 456 次执行，产生 0 个崩溃样本；cov 从 2243 升到 2274，新增单元 352 个，峰值 RSS 327 MB。
  - 运行参数：`-rss_limit_mb=800 -malloc_limit_mb=600 -max_len=16384 -timeout=30`。

## 结论与限制

- trailer 压缩带来的整包节省和 DEV-041 的估算一致，误差不超过 38 B，单文件提取也没有变慢。
- 加上 256 MiB 字典以后，SeedArc 在五棵 dev 树上的体积都是最小。
- **限制**：
  - 时间只跑了一次或五次，而且是在本机测的；
  - 只用了 dev 数据，不是独立评估；
  - 256 MiB 字典在大树上要 2.8–4.2 GB 内存（DEV-043）；
  - 多代归档测了追加和合并的形式保持，但没有测"多代压缩 trailer 一共省多少"；
  - 带字典 ID 的 zstd 帧没有单独做故障用例。它由 `decompress_solid` 原有的检查拒绝，固实帧的测试已经覆盖了这条检查。
