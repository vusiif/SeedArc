# ADR-0011：配方增加 LZMA2 字典字段

状态：**已接受**（用户 2026-11-28："加字典字段"），已在研究工具中实现。日期：2026-11-28。起草：主 Agent（Claude）。决定人：用户。

本 ADR 扩大 ADR-0006 配方映射修订 3 的允许集合，按 AGENTS.md 第 3 节属于 C 级，用户已明确授权。方式与 ADR-0007（extreme）、ADR-0008（dedup）相同：只把原先必须为 0 的位变成合法的值，已有的 Seed 一个都不改变含义。容器修订和解码方式都不变。

## 输入

- [DEV-042](../../research/experiments/20261128-7zip-version-main-001/record.md)：
  - 7-Zip 26 比 23.01 小，原因全部在于 `-mx9` 的默认字典从 64 MiB 改成了 256 MiB。
  - 用同一个 liblzma（也就是 SeedArc 用的那个），xz 换上 256 MiB 字典后，rustup 小 9.2%，cargo 小 4.7%。
- SeedArc 的 `lzma2-9e` 用的是预设 9 自带的 64 MiB 字典，所以整体固实的配方大小和 tar|xz `-9e` 相当（DEV-040）。

## 决定

### 1. 字段

- LZMA2 专用的 `dict` 码，3 位：
  - 0：沿用预设自带的字典；
  - 1 / 2 / 3 / 4：128 / 256 / 512 / 1024 MiB；
  - 5–7：保留，拒绝。
- 位置：
  - Seed 的 bit 17–19。保留位改为 bit 20–31。
  - 显式表示第二个字的 bit 18–20。保留位改为 bit 21 及以上。
- 文本形式：写在级别后面，例如 `solid/lzma2-9e-dict256MiB/g1-r0/frame-whole`。
  - 解析时必须严格匹配规范拼写。小写的 `mib`、带前导零、单位用 GiB、没有数值、把 `e` 写在字典后面，都会被拒绝。
- Zstd 配方带上非 0 的 dict 码，拒绝。

### 2. 只影响编码端

- 编码器实际用的字典是 min(所选字典, 帧的输入大小)。现有规则本来就把字典限制在输入大小以内，这条不变。
- 解码器的字典本来就等于帧的输出大小，任何匹配距离都在范围内。所以解码路径不需要任何改动，旧的读取器也能解出新写的包。
- 和 level、extreme 一样，`describe` 会写明字典是"declared, not verified from data"，也就是由写入方声明，没有从数据中核实。

### 3. 资源

- `solid_encoder_memory` 和 `memory-estimate` 都按实际选用的字典计算。整体帧时的估算：
  - 预设 9e 默认字典：每个线程约 2.85 GB；
  - 256 MiB：约 4.97 GB；
  - 1024 MiB：约 13.4 GB。
- `--jobs auto` 按估算减少线程数（ADR-0009）。连一个线程都放不下时，仍然开一个线程，并在报告里写明 `memory_over_budget`；配方不会被自动降级。
- 分配失败时，liblzma 返回内存错误，工具报 `resource_limit` 并失败，不会写出归档。
- 一个配方最多需要多少内存，由用户的选择决定；选码时要参考 `memory-estimate` 的结果。

### 4. 用法提示（不是规则）

- 字典比帧还大，没有任何用处：32 MiB 的帧配 256 MiB 的字典，结果和不加完全相同。字典应该和整体帧或大帧档一起使用。
- 推荐哪一档作为默认，等 dev 上测出体积、时间、内存三方面的数据以后再定；本 ADR 不改默认配方。

## 验收

- `research.solid_recipe`：
  - **测试树**：新增的 `far` 树里，`r2` 和 `r0` 内容相同，两者相隔 4 MiB。加了 128 MiB 字典以后，包至少小 1.5 MiB；冒烟测试里小了 2 096 024 B。
  - **一致性**：Seed 和显式两种表示的载荷相同，十六进制 Seed 和文本形式对应同一个配方；describe 输出带字典声明。追加以后字典保留；合并结果和直接打包逐字节一致；`memory-estimate` 的结果随字典增大。
  - **拒绝**：
    - Zstd 配方带 dict 码（Seed 和显式两种表示都测）；
    - 保留位（Seed bit 20、显式 bit 21）；
    - Seed 中 dict 码为 5；
    - 8 种错误的文本拼写。
- fuzz 语料新增 `r5-recipe-dict-seed.sadx` 和 `r5-recipe-dict-explicit.sadx`，回放通过。
- 构建：MSVC 50/50、WSL GCC 51/51、Clang ASan+UBSan 22/22，均为 0 警告。3 个固定小包在 GCC 和 MSVC 下的 SHA 相同。dev 测量见 [DEV-043](../../research/experiments/20261128-lzma2-dict-main-001/record.md)。
