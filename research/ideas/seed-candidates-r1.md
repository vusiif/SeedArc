# Seed 计划候选 A / B：修订 1

日期：2026-11-12。状态：**实验原型规则，尚未选型，不是归档协议**。本文件定义可复现的候选行为，用户体验与压缩收益仍待测试。

## 范围与修改边界

原型只生成拟议块计划，不读取文件、不压缩数据、不改变现有归档，也不参与解压。生产 CLI 仍拒绝未实现的 seed 策略。生成规则在 `core/seed/`，单独的 `seedarc_seed_study` 静态目标供测试与研究工具使用，不链接进生产归档库。

候选身份为 `(candidate_id, revision)`：A 为 `(1,1)`，B 为 `(2,1)`。未知 ID、未知修订或与候选不匹配的字段类型均拒绝。改变这些规则时增加修订，保留旧向量及相应代码版本，不能直接覆盖含义。

接口接收语义字段，不传递固定 16 字节的归档种子；研究工具的十六进制输入也只是候选字段编辑格式。以后更换表示或字段宽度，不要求修改 codec、I/O 或解压实现。

## 共同输入与混合规则

`block_id` 是固定规范化块清单内从 0 开始的全局序号，类型为无符号 64 位。它与具体路径或内容不绑定；前面插入一个块会改变后续身份。第一轮只验证固定清单，不声称满足文件插入 / 重命名隔离。

全部运算在无符号 64 位上进行，乘加按模 `2^64` 截断，右移为逻辑右移。定义 `M(x)`：

1. `x = x + 0x9e3779b97f4a7c15`
2. `x = (x XOR (x >> 30)) * 0xbf58476d1ce4e5b9`
3. `x = (x XOR (x >> 27)) * 0x94d049bb133111eb`
4. 返回 `x XOR (x >> 31)`。

这是 SplitMix64 的一步运算，原作者参考与许可见 [Sebastiano Vigna 的参考源](https://prng.di.unimi.it/splitmix.c)。本研究只把它作为无状态混合，不继承参考生成器针对连续状态的统计性质，更不用于密码学。

域常量为 `Dcodec = 0x636f6465632d7231`、`Dlevel = 0x6c6576656c2d7231`。定义 `R(s,i,d) = M(s XOR M(i XOR d))`，每次生成独立于调用顺序，不维护可变 PRNG 状态。

算法选择为 `R(s,i,Dcodec) % 3`：0→Store，1→Zstd，2→LZMA2。取模映射有微小偏差；修订 1 不宣称严格均匀或独立采样。未来改成拒绝采样需新修订。

## 候选 A：非结构化对照

语义输入：`value` 为任意无符号 64 位值。用它生成 codec。令 `r = R(value, block_id, Dlevel)`：

- Store 的拟议配置为 0；
- Zstd 等级为 `1 + r % 19`；
- LZMA2 预设为 `r % 10`。

同一个字段同时影响算法分配和参数，不承诺修改局部性。

## 候选 B：字段隔离

语义输入：`codec` 为任意无符号 64 位值，`zstd_rank` 在 0..18，`lzma2_rank` 在 0..9。用 `codec` 字段生成算法分配：

- Store 配置为 0；
- 所有 Zstd 块的等级为 `zstd_rank + 1`；
- 所有 LZMA2 块的预设为 `lzma2_rank`。

改变一个 rank 不改变 codec，且不改变另一个 codec 的参数。在合法范围内 rank 加 1，对应等级也加 1，不会绕回边界。**B-r1 每个 codec 只使用一个全局等级，表达能力比 A-r1 受限**：虽然二者支持相同的 codec 和等级范围，并不能据此认为具有相同的策略集合或采样分布。本轮用于验证语义隔离，不用于宣称同预算搜索优劣；后续公平搜索对照须匹配能力及分布，或明确把表示限制作为变量。

所有拟议输出都检查 codec / 等级合法性。原型没有实际压缩结果；接入压缩后还需单独报告 Store 回退的实际 codec，不能把计划当作实际执行结果。

## 固定向量

A 的十六进制 seed 与 block ID：

| value (hex) | block_id (hex) | codec | level |
| --- | --- | --- | --- |
| 0 | 0 | lzma2 | 2 |
| 0 | 1 | zstd | 2 |
| 0 | 2 | store | 0 |
| 0 | 3 | zstd | 9 |
| 1 | 0 | store | 0 |
| ffffffffffffffff | ffffffffffffffff | store | 0 |
| 0123456789abcdef | 2a | store | 0 |
| ffffffffffffffff | 0 | zstd | 2 |

B 输入 `(codec=0,zstd_rank=2,lzma2_rank=6)` 对块 0..3 的计划依次为 `lzma2/6, zstd/3, store/0, zstd/3`。这些期望值固定在单测中；主线程负责跨工具链执行，不把尚未运行写成通过。

## 构建与人工比较

默认不构建研究工具。使用现有 preset 时显式开启（先按开发说明设置 `VCPKG_ROOT` 等本机变量）：

```powershell
cmake --preset msvc-vs2026-release -DSEEDARC_BUILD_RESEARCH_TOOLS=ON
cmake --build --preset msvc-vs2026-release --target seedarc_seed_plan seedarc_seed_strategy_tests
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_plan.exe --help
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_plan.exe a-r1 0 16 1
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_plan.exe b-r1 0 2 6 16 0 3 6
```

字段使用 1..16 位十六进制，不含 `0x`；块数使用十进制 0..65536。B 输入中 `12` 表示十六进制 18，是最高合法 Zstd rank；超界与不完整输入退出 2。工具流式输出有界块数的 JSON，包含候选身份、语义字段、逐块拟议计划、可选比较计划与变化计数；`plan_only=true` 明确没有执行压缩。

第二条比较只把 Zstd rank 从 2 改为 3：Zstd 块应从等级 3 变为 4，其他块不变。A 把 seed 从 0 改为 1，则允许算法及参数同时变化。先请用户判断这些变化是否符合预期，再决定 B 是否需要逐文件或块段作用范围。

## 当前验证边界

新增测试覆盖固定向量、未知候选 / 修订、字段类型与等级边界、最大块序号、正序 / 逆序生成一致以及 B 两种参数的完整合法范围隔离。原型未实现 C、文件级身份、搜索、归档种子序列化或 GUI。未运行压缩实验，也没有压缩率或独创性结论；实际构建、运行和 IDE 证据由协调者写入 STATUS 所链接的验收 / 实验记录。
