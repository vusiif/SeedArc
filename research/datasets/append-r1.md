# append-r1

DEV-017 的追加实验语料，用来检验 Seed 的追加和解码优势，见 [seed-append-r1](../ideas/seed-append-r1.md) §7.1。清单与来源见 [JSON](append-r1.json)，建库和检查入口是 `research/scripts/append_corpus.py`。数据来源由用户 2026-11-15 选定为“本机 + 公开”混合，三个公开下载都已获得同意。

## 组成（2026-11-15 构建）

| 池 | 划分 | 文件 | 字节 | 组 | 初始 / 追加 1 / 2 / 3（文件数） |
| --- | --- | --- | --- | --- | --- |
| linux-src（公开，Linux 6.12） | dev | 3,625 | 50,331,389 | 71 | 2,520 / 353 / 368 / 384 |
| | holdout | 4,836 | 50,331,607 | 103 | 3,261 / 527 / 524 / 524 |
| native-bin（本机，MSVC / MSYS2 二进制） | dev | 63 | 67,072,886 | 42 | 43 / 9 / 7 / 4 |
| | holdout | 47 | 67,103,033 | 46 | 24 / 4 / 8 / 11 |
| silesia（公开） | dev | 5 | 74,384,271 | 5 | 2 / 1 / 1 / 1 |
| | holdout | 7 | 137,554,309 | 7 | 4 / 1 / 1 / 1 |
| wiki-text（公开，enwik8 切段） | dev | 50 | 50,000,000 | 50 | 35 / 5 / 5 / 5 |
| | holdout | 50 | 50,000,000 | 50 | 35 / 5 / 5 / 5 |

合计 8,683 个文件；dev 约 241.8 MB，holdout 约 305.0 MB。

## 规则

- **划分**：先把共享任何内容 SHA256 的组合并成连通分量（与 r2 相同），再按 `sha256("append-r1:<分量>")` 首个十六进制位的奇偶，整个分量进 dev 或 holdout。wiki-text 例外：前 50 段进 dev，后 50 段进 holdout。`--check` 从已提交的清单核对：dev 和 holdout 共享的内容为 0。
- **上限**：linux-src 每个划分 48 MiB，native-bin 64 MiB（取整组，第一个放不下的组跳过，所以实际略超）。silesia 和 wiki-text 不设上限。
- **路径**：
  - 每条路径都用 `seedarc_path_check` 检查（ADR-0005）。被拒 3 条：Linux 的 `drivers/.../i2c/aux.c`、`aux.h` 和 `include/soc/arc/aux.h`，`AUX` 是设备保留名。
  - Python `casefold()` 预过滤，排除了 26 个可能大小写冲突的文件，如 netfilter 的 `xt_CONNMARK.h` / `xt_connmark.h` 成对排除；名单在 JSON 里。
  - 每个划分的完整清单（含隐含的父目录）都通过了生产清单检查（`--manifest`：重复、大小写冲突、文件与目录重名）。
- **排除项**：空文件（linux-src 27 个）；符号链接（解包时只保留普通文件）；native-bin 只收 64 KiB – 32 MiB 的文件，也不收已压缩过的格式。
- **追加批次**：
  - wiki-text 按段的自然顺序；
  - 其他池按 `sha256("append-r1:<路径>")` 排序，按字节累计，前 70 % 为初始集，之后三个 10 % 各成一批；
  - 文件不超过 12 个的划分（silesia），最后三个文件各成一批。
- **enwik8 切段**：按顺序切成 100 段、每段 1,000,000 字节，是为了模拟“新数据不断追加”而人为构造的，不是原始文件结构。

## 存放与许可

- 原始文件只放在被忽略的 `research/local/append-r1/files/<划分>/<池>/` 下，仓库里只有清单（相对路径、大小、SHA256、组、批次）。
- 下载文件和解包清单在 `research/local/append-r1/{downloads,raw,listings}/`；Linux 源码解在 WSL 的 `~/corpora/linux/`。
- 许可：
  - Linux 是 GPL-2.0；Silesia 各文件来源不同；enwik8 是 CC BY-SA；
  - 本机二进制属 Microsoft 或各软件包，只在本机评估、不再分发。
  - 本机那一部分不能保证逐字节重建；三个公开来源可以按 JSON 里的 SHA256 复现。

## 使用记录

- 尚未使用。holdout 只在 DEV-017 的配置冻结之后评估一次。
