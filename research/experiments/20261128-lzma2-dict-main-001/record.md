# LZMA2 字典字段的实现与 dev 测量（DEV-043）

2026-11-28，主 Agent（Claude），无委派。状态：执行者报告，**未经独立审计**。依据：[ADR-0011](../../../docs/decisions/0011-recipe-lzma2-dictionary.md)（用户已授权）。

## 问题

[DEV-042](../20261128-7zip-version-main-001/record.md) 发现，7-Zip 26 的优势全部来自 256 MiB 的字典。本实验要回答三个问题：

1. 给 SeedArc 的配方加上字典字段以后，在 dev 树上实际能小多少？
2. 时间和内存的代价是多少？
3. 和外部工具相比，结果如何？

## 实现

- **配方**（research/tools/solid_plan.hpp）：
  - 新增 LZMA2 字段 `dict_code`：0 表示用预设的字典，1–4 分别是 128 / 256 / 512 / 1024 MiB，5–7 保留。
  - 存放位置：Seed 的 bit 17–19，或显式表示第二字的 bit 18–20。
  - 文本写法：`lzma2-9e-dict256MiB`。
- **编解码**（core/codec/dictionary.*）：
  - `compress_solid_codec` 和 `solid_encoder_memory` 新增参数 `dict_size`，合法性由 `valid_solid_dictionary` 检查；
  - 两个函数共用 `lzma2_encoder_options`。实际使用的字典取 min(选定值, 输入大小)；
  - 解码不需要改。
- **工具**：
  - append4_study 和 memory-estimate 都把字典传给编码器；
  - describe 输出里多一项 "level and dictionary declared"。
- **测试**：
  - check_solid_recipe.cmake 新增一节"字典"：用 `far` 树打包，Seed 和显式两种表示结果一致，追加后字典保留，合并结果和直接打包逐字节一致，memory-estimate 随字典变化。
  - 拒绝用例新增 Zstd 带字典、保留位、Seed 码 5、8 种错误拼写。
  - 原先测保留位用的 Seed bit 17 / 显式 bit 18，改为 bit 20 / bit 21；原先的拒绝 Seed `0x22500` 现在是合法值，改成 `0x102500`。
  - recipe_fixture.py 新增 `far` 子命令。
- **fuzz 样本**：
  - `r5-recipe-dict-seed.sadx`：`solid/lzma2-1-dict256MiB/g1-r0/frame-whole`，877 B；
  - `r5-recipe-dict-explicit.sadx`：`solid/lzma2-6e-dict128MiB/g2-r1/frame-16MiB`，977 B。

## 验证

- **MSVC Release**（build/dictstudy-check）：构建 0 警告，CTest 50/50。其中 `research.solid_recipe` 用时 433 s。
- **WSL**：工作树快照 `08424a6`，临时 ref 用完已删。
  - GCC Release：0 警告，CTest 51/51；
  - Clang ASan+UBSan：0 警告，相关的 22/22 通过，没有 Sanitizer 报告。
- **fuzz 语料回放**：19 个输入（17 个原有 + 2 个新增），退出码 0。**本轮没有新跑 fuzz。**
- **跨工具链**：GCC 和 MSVC 打出的 3 个固定小包，SHA256 都相同：
  - 两个 fuzz 样本：`59120c7b…`、`daf28be6…`；
  - `far` 树 `solid/lzma2-0-dict128MiB/g1-r0/frame-whole`：`364a994a…`。
- **CLion**：按 STATUS 的约定，本阶段暂缓；MCP 也连不上。没有运行。

## dev 测量

- **方法**：
  - 机器：本机 Windows，`--jobs 1`，没有固定 CPU。
  - 工具：上面那个 MSVC 构建，SHA256 `1a2a5ebb…`。
  - 树：和 DEV-040 是同一批，已与上传清单核对过。
  - 每个包写出后都用 `verify` 逐字节比对；峰值内存取进程的峰值工作集。
- **脚本**：仓库外的 `dictmeasure.py`，内容抄在 `commands.md`；原始数据在 `local/raw.jsonl`。
- **跨平台核对**：不加字典时，四棵树的字节数和印度的 Linux 结果完全一致。

| 树 | 不加字典 | 256 MiB | 变化 | 1024 MiB | 打包峰值内存：不加 → 256 MiB |
| --- | ---: | ---: | ---: | ---: | --- |
| append-r1 dev | 35 612 206 | 35 580 642 | −0.09% | 35 580 642 | 1 324 → 2 759 MiB |
| cargo | 20 168 897 | 19 426 153 | −3.68% | 19 426 153 | 1 388 → 2 973 MiB |
| rustup | 48 727 168 | 44 311 933 | −9.06% | 44 311 933 | 1 386 → 3 040 MiB |
| 全部 | 70 687 543 | 65 435 937 | −7.43% | 65 409 457（−7.47%） | 2 145 → 4 161 MiB（1024：6 203） |

- 前三棵树都不超过 256 MiB，所以 1024 MiB 实际上被截到了输入大小，结果和 256 MiB 完全相同。
- **时间**：打包时间在 ±15% 以内，各有快慢，属于单次测量的波动，不能下结论。
- **解码**：verify 的峰值内存没有变化（685–1 540 MiB），因为解码器本来就按帧的大小分配字典。

### 和外部工具比（体积，字节）

| 树 | SeedArc 256 MiB 去重 | 再扣 trailer 的估算（DEV-041） | 7-Zip 26.03 -mx9 | xz 256 MiB 字典（DEV-042） | tar \| xz -9e |
| --- | ---: | ---: | ---: | ---: | ---: |
| append-r1 dev | 35 580 642 | 35 375 981 | 36 065 458 | — | 35 445 856 |
| cargo | 19 426 153 | 18 553 286 | 18 931 714 | 18 619 240 | 19 539 944 |
| rustup | **44 311 933** | 44 287 307 | 46 553 189 | 44 312 736 | 48 782 200 |
| 全部 | **65 435 937** | 64 384 009 | 67 016 273 | — | 69 937 464 |

- **现在就已经领先**：rustup 比 7-Zip 26 小 4.8%，全部小 2.4%，append-r1 dev 小 1.3%。
- **只有 cargo 还大 2.6%**：这棵树文件多，未压缩的 trailer 有 1.09 MB。扣掉 trailer 的估算以后，cargo 也比 7-Zip 26 小 2.0%，比 xz 256 MiB 小 0.4%。
- 在 rustup 上，SeedArc 和"xz 加同样字典"只差 803 B，说明容器本身几乎没有额外开销。

## 结论与限制

- 加了 256 MiB 字典的整体去重配方，在四棵 dev 树中的三棵上已经是本轮所有工具里最小的。再实现 ADR-0010 的 trailer 压缩（按估算），四棵树就都是最小。
- **代价是内存**：打包峰值从 1.3–2.1 GB 升到 2.8–4.2 GB，1024 MiB 字典压"全部"时达到 6.2 GB。在 6 GB 内存的机器上，`--jobs auto` 只能开 1 个线程，大树会跑不动。默认用哪一档，应该交给偏好档决定，本次不改默认配方。
- **限制**：
  - 每个格子只跑了一次，时间没有重复测量；
  - 只用了 dev 数据，不是独立评估；
  - trailer 压缩后的体积是估算；
  - 7-Zip 和 xz 的结果分别来自不同的运行，不过输入是同一批树，体积可以直接比较；
  - 128 MiB 和 512 MiB 两档在 dev 树上没有测；
  - 本轮没有新跑 fuzz，只回放了语料。
