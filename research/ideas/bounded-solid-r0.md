# 有界固实帧：联合取舍首轮基线

2026-11-24，DEV-021。依据[用户可选择的联合目标](speed-resilience-r1.md)，先测分段本身对体积、编码 / 追加、单文件提取与载荷损坏影响的取舍，未冻结用户预设。

## 最小实现与兼容性

继续调用既有Zstd-9固实适配层，不复制第三方codec，不修改v4/v5头部、记录或索引。既有帧表已经记录每帧长度 / 偏移和每文件帧引用；写入器在文件之间截断一组的拼接序列，生成多个独立帧。同一组可有多个帧，文件仍完整位于一个帧中。

首轮帧上限为32 / 64 / 128MiB，另以既有1GiB资源上限作为整段对照。单文件超过所选上限明确拒绝，不自动放宽或分割文件；全空内容保留1字节帧占位，文件逻辑长度仍为0。帧上限是原文长度，不承诺压缩后含索引字节也小于该数。总输入 / 文件数 / 路径 / 解码窗口限制保留，仍整树 / 整归档装入内存，不是完整流式实现。

当前Seed映射revision2继续只表示既有分组计划；帧上限是显式运行配置，不把现有Seed值静默解释为新布局选择。归档中的帧表足以解码，上限不作为新协议字段。解码器不变；修改前读新包和默认旧包字节相同必须实际验证。

## 私有研究工具入口

```text
seedarc_append4_study bounded-pack seed|explicit FRAME_LIMIT_BYTES INPUT_DIR OUT
seedarc_append4_study bounded-append FRAME_LIMIT_BYTES OLD NEW_DIR OUT
seedarc_append4_study access-profile ARCHIVE INPUT_DIR UNIFORM_SAMPLE_COUNT
seedarc_append4_study payload-fault-profile ARCHIVE INPUT_DIR
seedarc_append4_study extract ARCHIVE PATH OUT_DIR BASENAME
seedarc_append4_study extract-profile ARCHIVE INPUT_DIR UNIFORM_SAMPLE_COUNT EMPTY_OUT_DIR
```

### 索引式单文件提取（DEV-024，2026-11-25）

`extract` / `extract-profile` 是真实落盘的单文件提取，用来补 access-profile"整包读入内存"的缺口。

- **只读需要的部分**：用定位读取，只读头部 32 B、尾部 footer 24 B、trailer、每个 GEN 记录（与 trailer 副本逐字节比对），再读目标文件所在帧的压缩字节。每个读取的字节都计数。
- **只解到目标文件为止**：帧只解码到目标文件末尾；空文件不解码。之后校验文件 CRC32C。
- **安全落盘**：经 SafeOutputDirectory 写出：独占临时文件，commit 时复核大小和 CRC，重命名时不替换已有文件。任何检查失败都不发布输出。
- **校验范围比全量解析小**：不做全量解析里"逐记录扫描交叉核对"这一步。保护结果的只有：trailer CRC、头部 CRC（存在 trailer 里）、GEN 副本一致、trailer 自身的边界检查、文件 CRC32C。这是有意的取舍：按索引提取本来就不读全部载荷。全包完整性仍用 verify。
- **适用范围**：只支持修订 5 固实包，v4 等其他格式明确拒绝。
- **extract-profile**：样本规则与 access-profile 相同（排序路径均匀位置加最大文件，去重）。每个样本都重新打开归档、读索引、提取到空目录，全程计时；计时之外再与参考输入逐字节比对，然后删除输出。分项计时包括索引读取 / 解析、帧读取、解码、写出 + commit。
- **限制**：没有 fsync，不清 OS 缓存，所以结果仍是热缓存。帧的压缩字节整段读入，没有流式只读前缀。解码缓冲最大等于帧原文大小，受帧上限约束。
- **Windows 参数编码**：append4_study 在 Windows 上改为从宽字符命令行取 UTF-8 参数。原来的 ANSI argv 传不了中文等非 ASCII 路径；ASCII 参数的行为不变。

测试 `research.bounded_extract` 覆盖：
- **逐字节一致**：4096 B 和整段两种帧档下，每个文件（含空文件、恰等于上限的文件、中文目录 / 文件名）都逐字节提取一致；4096 B 档下实际读取量小于包大小。
- **采样一致**：extract-profile 与 access-profile 选中的样本相同，profile 结束后输出目录为空。
- **失败都不留输出**：不存在的路径、`../` 和带 `/` 的输出名、已存在的输出（原内容保留）、非空的 profile 目录、载荷 / trailer / 头部翻转、截断、v4 包、参数错误。

bounded-pack固定单组 / 无字典 / off冗余，两种表示使用同一帧编码和序列化路径。bounded-append仅接受revision5固实归档，旧帧也必须满足所选上限；新路径追加保留完整旧包前缀，旧原文不重压，但旧压缩包仍完整读取、复制和重写，成本明确计入。旧帧超限时拒绝，改变旧布局需要另行重建，不能声称无成本切换档位。

access-profile使用均匀排序路径位置并加入最大文件（去重），每个样本重新读取和解析完整归档，解码其所在帧，再校验CRC及参考源的逐字节相同。输出每个路径的请求 / 实际解码 / 包读取字节、时间与分位数。源输入加载不计入样本计时，包读取、解析、解码和校验计入；峰值RSS包含参考源，不冒充生产extract内存。没有选择性磁盘I/O或decoder缓存加速，也没有写出实际提取文件；该入口是可复现提取成本诊断。

payload-fault-profile在任务生成包的内存副本上，对压缩载荷累计10/50/90%位置各翻转1bit；每次重新解析受损包，解析失败不得使用原始索引绕过。只对全帧解码成功且逐文件CRC / 参考字节匹配的内容计可提取量，所有错误和不可用字节均保留，不写出数据。假设索引 / 头部完好，不覆盖缺卷、元数据损坏或前缀流式抢救；v5 partial / split接口仍明确拒绝。这里的结果不是通用恢复能力或认证加密承诺。

## 实验与评价

首轮沿用mixed dev的initial+3次新路径追加，各帧上限和两种表示有独立历史，最终再从相同完整输入重建对照。报告每次与累计编码成本、完整归档字节 / 索引增长、旧包读写 / 重压量、单文件提取分布、实际与潜在受损帧覆盖量。必须计入必要整理 / 全量重建，不用一次包大小代替整个生命周期。

窗口规则仍为`min(27,ceil(log2(frame bytes)))`，帧大小变化会按同一规则改变实际窗口。不同帧上限体现该完整策略的实际取舍，不能把收益归因于Seed表示。固定Zstd9是隔离候选维度的首轮基线，其他codec / 等级及相同时间预算的竞争曲线仍是后续工作；没有因此声称优于所有压缩算法。

真实验收、固定源码 / 输入 / 运行环境、单遍dev证据与限制见[本轮记录](../experiments/20261124-bounded-solid-main-001/record.md)。尚未据此实现正式CLI预设 / GUI选择或宣布某档最优。

稳定性复测入口：[DEV-022四档四轮平衡顺序](../experiments/20261124-bounded-repeat-main-001/record.md)。C++ / codec / 输入不变，固定第3代父包，多轮重新执行末批追加与全量重建，以同轮整段对照计算倍率，保留首次准备 / 整理成本。复测仅覆盖既有mixed dev与热缓存，不能替代其他数据、冷磁盘或更大规模验证。

## 四轮复测后的研究选项

[DEV-022观测表](../experiments/20261124-bounded-repeat-main-001/preview.md)中，32MiB四轮同轮提取诊断倍率为2.90–3.33，64MiB为2.07–2.33；体积增幅分别0.470% / 0.050%。压缩调用没有清晰的大幅提速，故选项名称强调单文件读取，而非“极速压缩”。

可追溯选项表为[bounded-solid-options-r0.json](../configs/bounded-solid-options-r0.json)：整段体积参考、64MiB低额外体积兼顾读取、32MiB偏单文件读取 / 较小帧，另保留128MiB手动取舍。没有通用默认，没有自动择优或正式CLI / GUI集成；这是现有私有研究工具的参数映射，性能数值仅指链接的同dev观测，不是其他输入的包增幅 / 时间保证。

手动使用对应现有入口，例如64MiB：

```text
seedarc_append4_study bounded-pack seed 67108864 INPUT_DIR OUT
seedarc_append4_study bounded-append 67108864 OLD NEW_DIR OUT
```

32MiB使用33554432，128MiB使用134217728，整段参考使用1073741824。超上限单文件和超上限旧帧仍拒绝；已有整段包改为小帧需要显式重建，不能无成本切档。所有档均支持既有新路径追加；本轮末批18.39MB小于最小32MiB帧上限，因此追加时间主要体现避免旧原文重压，不证明小帧更适合所有批次大小。修改 / 删除旧路径、缺卷恢复、多codec速度档仍不在已验收范围。
