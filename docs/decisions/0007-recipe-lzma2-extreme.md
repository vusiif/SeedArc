# ADR-0007：配方允许集加入 LZMA2 extreme

状态：**已接受（用户 2026-11-26 选择"Accept, implement"，三项均按推荐：加入、方案 A、全部预设 0–9）；已在研究工具中实现**，见 §7。日期：2026-11-26。起草：主Agent（Claude）。决定人：用户。本 ADR 修订 [ADR-0006](0006-seed-solid-recipe.md) 的允许集（§2.1）和位布局（§2.3），ADR-0006 的其他内容不变。格式仍为 experimental、未冻结；只在研究工具中实现，不改正式归档格式。

输入：
- DEV-033 实测（[记录](../../research/experiments/20261126-codec-capabilities-main-001/record.md)、[设计草案 §5](../../research/ideas/codec-capabilities-r0.md)）：
  - 按 32 MiB 分帧的完整归档，LZMA2-9e 比 LZMA2-9 小 2.72%（源码）和 2.08%（文本）；LZMA2-6e 比 LZMA2-6 小 2.8% 和 1.9%；
  - 代价是编码慢 1.4–1.7×，解码时间和内存不变；
  - 整帧测量和 native-bin（仅本机评估）的结论方向一致。
- 用户对 Seed 的定位（[user-vision §4](../user-vision.md#4-seed-应承担什么作用)）：Seed 让人知道自己在用什么压缩，并固定这种压缩方式。
- 用户 2026-11-26 的指示："Yes, draft it"，即起草把 extreme 加入配方的方案。

## 1. 问题

LZMA2 extreme（liblzma 的 `LZMA_PRESET_EXTREME`）只改变编码器的匹配搜索：输出仍然是同一种 LZMA2 原始流，现有解码器不需要任何新参数就能解。

但它确实改变了压缩方式和结果。如果配方里不写出来，就会出现下面的情况：
- 两个包的 Seed 相同，压缩结果却不同；
- `describe` 报出的配方与实际编码方式不符。

这违背 ADR-0006 §2.4"标签必须真实"的要求。所以要用 extreme，就必须把它放进配方。

## 2. 决定（候选）

### 2.1 允许集

| 字段 | 现在（ADR-0006） | 修订后 |
| --- | --- | --- |
| LZMA2 | 预设 0–9 | 预设 0–9，每个预设可选 extreme |
| Zstd | 级别 1–19 | 不变；extreme 对 Zstd 无意义，一律拒绝 |

可读文本：`lzma2-<预设>e`，例如 `solid/lzma2-9e/g1-r0/frame-32MiB`。只有这一种拼写；`lzma2-9E`、`lzma2-9-e`、`zstd-9e` 都拒绝。

### 2.2 位布局（推荐方案 A：新增 1 位）

```text
bit  0–14  与 ADR-0006 §2.3 相同
bit 15     extreme：1 = LZMA_PRESET_EXTREME；只在算法码 = LZMA2 时允许为 1
bit 16–31  必须为 0
```

显式两字描述符：第二字的 bit 16 作为 extreme 标志，bit 17–31 必须为 0。

这样做的理由：
- 和 2026-11-25 扩大允许集一样，只是把原先非法的值变成合法：现在 bit 15 必须为 0，所有已合法的 Seed 含义不变，映射修订仍为 3；
- 旧工具遇到 bit 15 = 1 的 Seed 会按非法值拒绝，属于明确拒绝，不会误读成非 extreme；
- extreme 是一个独立的开关，单独占一位，看 Seed 的人不用查表。

### 2.3 备选（不推荐）

| 方案 | 做法 | 不推荐的原因 |
| --- | --- | --- |
| B：塞进级别字段 | LZMA2 级别 10–19 表示预设 0–9 加 extreme | 一个字段有两种含义，位值不再等于级别，可读性差 |
| C：占用新算法码 | 算法码 2 = "LZMA2 extreme" | 算法码只剩 2–3 两个，原计划留给 Brotli / LZ4；而 extreme 并不是另一种算法 |
| D：提升映射修订到 4 | 重新设计位布局 | 没有必要；方案 A 已经兼容 |

### 2.4 核对与 `describe`

- 和级别一样，extreme 无法从压缩数据验证，只能重新压缩比对，代价太高。`describe` 把它列为声明值，与级别一起写明"未从数据验证"。
- 帧边界、算法的核对规则不变。

### 2.5 编解码器层

- 写入端：配方代改用一个接受 extreme 的入口，可以复用 DEV-033 的 `compress_solid_variant`，也可以给 `compress_solid_codec` 加一个参数。
- 读取端：不改。extreme 帧走现有的 `decompress_solid_codec` 和前缀解码，解码内存上限不变。
- 确定性：单线程，固定 liblzma 版本（5.8.4）时输出确定。和其他配方一样，只承诺同一 codec 版本下逐字节相同，不承诺跨 liblzma 版本相同。

## 3. 不做

- 不加 Zstd 多线程。它的输出与单线程不同，要作为"同一 zstd 版本下与线程数无关"的单独承诺另行决定；而且分帧本身就可以并行压缩。
- 不加 BCJ、按类分帧和强开 LDM：DEV-033 实测没有收益或收益太小。
- 不改默认配方。CLI 默认仍由以后的推荐规则决定，本 ADR 只扩大允许集。

## 4. 兼容与风险

- **旧包**：不受影响，没有任何已有 Seed 改变含义。
- **旧工具**：拒绝新值（fail closed）。
- **编码时间**：extreme 慢 1.4–1.9×，所以它只是可选项，不是默认。
- **位 15 一旦有正式数据就固定了含义**：以后想给 Zstd 也加编码器开关，只能用 bit 16 及以上，或者新的修订。

## 5. 验收（实现时）

- 允许集内每个 `lzma2-<n>e` 都能往返：Seed ↔ 文本 ↔ Recipe ↔ 显式表示；Seed 包与显式包载荷逐字节相同。
- 以下情况全部拒绝，被拒绝的命令不留输出：
  - Zstd 加 extreme（Seed、文本、显式三种表示）；
  - 拼写变体；
  - bit 16–31 非零。
- `lzma2-9e` 包能 verify、能索引式提取，也能 `recipe-append`、`consolidate`；`describe` 输出 `lzma2-9e`，并标注声明值。
- 与 DEV-033 的数据对账：在同一棵树上，`recipe-pack` 写出的 `lzma2-9e` 归档，数据部分等于 `codec-probe-framed` 的帧字节。
- MSVC、Linux GCC、ASan+UBSan 全部通过，0 警告；跨平台固定小包 SHA 一致，并新增一个 extreme 的固定小包。

## 6. 用户决定（2026-11-26）

1. 是否把 extreme 加入允许集？推荐：**加入**，作为可选项，不作为默认。——已采纳。
2. 用哪种表示？推荐：**方案 A**，bit 15 作为 extreme 标志，映射修订仍为 3。——已采纳。
3. 范围：LZMA2 全部预设 0–9，还是只允许 9e？推荐：**全部预设**。实测 6e 也有效，而且 6e 在源码上比 LZMA2-9 还小 1.4%，内存少 270 MiB。——已采纳。

## 7. 实现

- `research/tools/solid_plan.hpp`：`RecipeFields` 加 `extreme`；`valid_recipe` 只允许 LZMA2 带 extreme；Seed bit 15、显式第二字 bit 16；Seed 的保留位改为 16–31，显式第二字改为 17–31；文本 `lzma2-<n>e`，解析后必须原样打印回去。
- `core/codec/dictionary.*`：`compress_solid_codec` 加默认为 false 的 `extreme` 参数，非 LZMA2 带 extreme 时拒绝。解码函数未改。
- 写入端（`append4_study.cpp`）把配方的 extreme 传给编码器；`describe` 的 `check` 写明"level and extreme declared, not verified from data"。
- 测试：
  - `research.solid_recipe`：`lzma2-9e` 两种表示的往返与载荷一致、十六进制 Seed 与文本一致、追加保留 extreme、索引式提取、合并等于直接 `recipe-pack`；`lzma2-6e` 在 21 MiB 上的两帧提取与追加；
  - 拒绝：7 种拼写变体，以及 Seed `0x8000`、`0xa400`（Zstd + extreme）、`0x12500`（bit 16）；
  - 用 `recipe_fixture.py retag` 改写记录：Seed 为 Zstd 加 bit 15 或 LZMA2 加 bit 16、显式为 Zstd 加 bit 16 或第二字 bit 17，`verify`、`describe` 都报"bad solid recipe record"，`extract` 不写输出；
  - `research.codec_probe`：`recipe-pack lzma2-9e` 的数据部分等于 `codec-probe-framed` 的帧字节。
- 验证结果见 STATUS 的 DEV-034。
