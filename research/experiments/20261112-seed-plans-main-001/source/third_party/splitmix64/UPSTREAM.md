# SplitMix64 单步混合片段

- **用途**：Seed 候选 A-r1 / B-r1 的无状态确定性混合；只供研究目标与测试，不加入生产归档库。不是密码学 PRNG，也未验证本候选组合的统计质量。
- **作者 / 来源**：Sebastiano Vigna，2015；[官方 `splitmix.c`](https://prng.di.unimi.it/splitmix.c)，原 `splitmix64.c` 地址会重定向到该文件。取自该文件第 33–41 行（1-based）的状态与 `next()` 片段。
- **获取日期**：2026-11-12。来源是作者网站的文本响应；没有上游 release / tag / commit 信息。以本目录保存的 LF 规范化片段及 SHA256 固定本次输入，校验值是本地快照指纹，不冒充上游发布包校验。
- **许可**：原作者的 public-domain dedication，以及允许使用、复制、修改和分发的许可与免责声明完整保存在 [LICENSE](LICENSE)。未将其改标为项目 GPL，也未假定采用 CC0。
- **保存文件**：`upstream-fragment.c.txt` 保留公式与原状态函数；文件前的来源注释为本地增加。片段不独立编译。
- **本地适配**：`splitmix64.hpp` 改用 C++ 固定宽度类型和显式传值，移除全局可变状态，函数命名为 `seedarc::third_party::splitmix64_step`，添加 inline / noexcept / nodiscard 与 ULL 常量后缀；运算顺序、常量和结果不变。完整本地变换见 `stateless-cxx.patch`，无上游提交。
- **构建集成**：`core/CMakeLists.txt` 中 `seedarc_seed_study` 的私有 include 路径指向 `third_party`，适配器位于 `core/seed/strategy.cpp`。只包含头文件，无新包、ABI 或动态库依赖。
- **验证范围**：新增 A/B 固定向量、最大值与调用顺序测试；实际构建和运行由协调者统一验证，不能把这些测试的存在写成已经通过。
- **更新方法**：核对官方源与许可；保存新输入与哈希，重放或明确改写本地适配。若输出改变，先建立新的候选规则修订并保留旧向量，不能静默升级 r1；重新运行候选测试和跨工具链比较。

## 本地 SHA256（UTF-8 / LF）

| 文件 | SHA256 |
| --- | --- |
| upstream-fragment.c.txt | `1a853eb5c7be9c52861e79de3667295effbb9cc7ea33b89d6b4da72756e9eb26` |
| splitmix64.hpp | `0089c3db376171dd5f80979d8130c49e36815accd2da1db24ea66741c94fefa7` |
| LICENSE | `9d73c6155990baa6ea4647d15f382141271e6c665303dc550045bc8dd84fd388` |

Git 可能在 Windows checkout 时转换换行；复核这些指纹前先按 UTF-8 / LF 规范化，不把 CRLF 差异当成算法改变。
