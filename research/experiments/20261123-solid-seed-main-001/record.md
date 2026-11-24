# 成熟固实策略上的 Seed：零搜索接入

日期：2026-11-23（America/Los_Angeles），主 Agent，无委派；状态：实现及命令行 / 动态 / 大目录验证完成，CLion集中诊断按用户选择待补；不宣称全部阶段验收完成。

用户授权：在成熟固实 / 固实分段策略之上最小接入 Seed，小文件仅作正确性验收，重点进入已验收大规模。

方法预登记：沿用 v4 的 Zstd-9 solid 编码路径，新增实验容器头部修订 5（保留原记录标签族与 v4 读写）；仅支持 solid，先限制 off 冗余，不自动声称恢复 / 分卷可用。新增 seed-solid / explicit-solid；固实策略映射 revision2，仅低五位描述规则和 k（默认0为单组），不解释旧 G5 字典位段。显式形式用 layout=1、rule、k、level=9 四字节描述同一具体计划；Seed 与显式形式各代都保留可重建计划，直接消费相同编码管线。旧映射 revision1 及旧包字节不变。

检查：固定向量 / 非规范种子 / 未知修订拒绝；中文、空文件、多块与新组键；pack / segment / 新代计划 / regroup 的逐字节往返和确定性；Seed / 显式及单组旧 solid 的帧载荷逐字节一致；旧包读写兼容。不同表示的完整包大小与数据字节分开核算，不预设压缩收益。默认不搜索，本轮不把旧分块搜索器套到固实上。

源码基线为当前479eab9与已存在资源补丁，修改前 Windows 二进制 SHA256 为3f4c930895de33063ff2fa96f7d0e17a0f0ea973dc79a012a4b6093183a3b56b；后续保留补丁、工具哈希、原始日志。拟运行 Windows Release、印度 Release / ASan+UBSan，阶段末集中 CLion；真实512MiB公开dev仅在小验收通过后启动，不读取holdout、不抬高输入界限。用户原稿两份哈希已复核未变。

## 已完成本机验收与实际远端任务

Windows MSVC Release 构建成功；全量 CTest **43/43**（69.47 s），见 windows-ctest.log。新增定向脚本通过 seed-solid / explicit-solid / legacy-solid 在 4 个固定计划（0 / 6 / 11 / 31）下的逐帧压缩字节一致性、重复打包、segment / regroup、新代计划、非法位段 / 不支持操作拒绝；旧二进制对三种旧模式的字节一致及双向读取通过，见 windows-targeted.log。小夹具含中文路径、空文件、多块内容和新键，作为功能验收，不用于性能结论。

印度任务 `20261124T065628Z-509e5ae7` 已启动，exclusive，2小时上限，源码输入5,756,561 B；先Release全量和旧Linux二进制兼容，再ASan+UBSan Debug全量，删除本任务ASan可再生构建后才复制真实512MiB dev。工具链仍GCC14.2 / pinned zstd1.5.7、liblzma5.8.4，未系统安装工具。输入与源码zip哈希见 input-hashes.json。沿用任务专用磁盘监控修复，2GiB阈值不变，已安装skill未改；原/新监控哈希见monitor-patch.json。

大目录零搜索预登记：默认计划0，legacy-solid / seed-solid / explicit-solid三条各自历史，初始加3批追加，每包独立verify，两组载荷比较逐帧、逐文件布局相同；其后添加一份小型中文文件，在三条历史上分别regroup并verify / compare。预期40条工具命令、15个校验产物、10次载荷比较，0次搜索。保留各阶段完整包SHA / 字节构成，旧包前缀 / 哈希验证后回收；不将结果当作搜索收益。单组强默认只验证封装等价，更高组数在小夹具验收。

CLion：用户本轮明确暂时不方便放前台，先做构建和测试；本轮集中诊断待补，不声称阶段全部IDE验收通过。没有重复请求焦点，也未以CLI检查冒充IDE。当前尚无Linux或大目录完成结果。

进度补充：印度 Release 全量 **44/44**（74.37 s）及旧 Linux 二进制兼容检查已报告通过，ASan+UBSan 正在运行，待最终归档独立核验。Windows 旧读取器对两种新修订5模式均以 bad header 明确拒绝，见 old-reader-new-profile-rejection.json，未静默当作旧格式读取。

动态检查补充：印度 ASan+UBSan Debug 全量已报告 **44/44**（403.00 s），零搜索大目录阶段已开始；原始包待取回核验。零搜索运行器会回收重组最终包，另启动仅只读的观察器，在各包verify成功后、回收前记录3份完整包SHA256；不改变已上传源码 / 输入哈希或策略，不将这些观测用作时间优越性结论。

## 最终结果与独立核验

印度任务于2026-11-24 06:58:36–07:17:13 UTC（当地2026-11-23）正常exit0，日志6,217 B未截断。fetch归档SHA256 `44fbde90b4c688ddc3444ca498a71707428e608fc4f4c328dbcb91e4b0b3cee4`已核验，归档 / 回执位于research/local/india-append/artifacts/。任务输入源码zip哈希与最终完成记录一致，ELF二进制为 `678285ce993ca5a1510db0e1670cf7aaedde8965d85df5fc7d4df00d86f32ec0`。

主Agent从归档提取并独立核对：Release **44/44**（74.37 s），ASan+UBSan **44/44**（403.00 s）；两份构建日志0 warning / error，完整ASan LastTest未发现AddressSanitizer / UBSan / LeakSanitizer报告。原始日志见 [Release](linux-release-ctest.log)、[Sanitizer](linux-asan-ctest.log)、[兼容](linux-legacy-compatibility.log)。本轮只有已有fuzz语料重放与新增定向坏元数据 / 损坏帧检查，没有新的持续fuzz运行。

新增独立跨平台固定夹具：4计划×2表示×初始 / segment / regroup / 新代计划，共**32份归档**，Windows/MSVC与Linux/GCC完整包大小和SHA256逐个完全一致，含中文路径和空文件。工具 / 夹具哈希见 [cross-platform.json](cross-platform.json)，原始清单见 [Windows](windows-cross/archives.json) / [Linux](linux-cross-archives.json)。这只覆盖固定小夹具，不宣称真实512MiB归档已在Windows重跑。

大目录数据沿用固定Linux6.12 dev：536,864,155 B、30,134文件，清单SHA一致、不读取holdout。本轮使用默认Seed0，四代（初始＋三批追加）独立延续三种表示；全部**40条命令退出0、15个产物逐字节校验、10次完整帧载荷 / 文件布局比较通过、0次搜索**。十二个分段历史包SHA由运行器保存，三份regroup包在verify之后、回收之前由仅只读观察器取得SHA（文件大小 / inode / mtime稳定），见 [regroup-hashes.json](regroup-hashes.json)。所有15份包都有哈希记录；不修改已上传的运行器或策略代码。

| 阶段 | 旧固实包 (B) | Seed固实包 (B) | 显式固实包 (B) | 载荷关系 | 新表示额外元数据 |
| --- | ---: | ---: | ---: | --- | ---: |
| 初始 | 39,195,542 | 39,195,550 | 39,195,550 | 逐帧相同 | 8 B |
| 追加1 | 48,062,342 | 48,062,362 | 48,062,362 | 逐帧相同 | 20 B |
| 追加2 | 56,885,999 | 56,886,035 | 56,886,035 | 逐帧相同 | 36 B |
| 追加3 | 65,818,218 | 65,818,274 | 65,818,274 | 逐帧相同 | 56 B |
| 再加一份小型中文文件后regroup | 60,705,558 | 60,705,566 | 60,705,566 | 逐帧相同 | 8 B |

差异来自每代增加4字节计划值，在代头 / 索引以及保留的旧尾部中重复保存；四代累计14个副本，共56 B。Seed与显式描述当前都为4字节，二者完整大小相同，不声称Seed表示已比显式描述更省。最大工具RSS为1,714,233,344 B，观测任务逻辑磁盘峰值1,727,870,358 B，没有提高既有输入 / 帧 / 任务磁盘限额。

分析脚本从原始JSONL检查索引连续、全部退出码、代数 / 独立verify、容器与策略修订、精确物理字节对账、载荷比较和每代8/20/36/56 B增量，主Agent验证输入清单 / 源码 / 二进制与跨平台证据哈希。原始数据在 [zero/](zero/)，机器汇总见 [analysis.json](analysis.json)。复现：

```text
python -B research/scripts/solid_seed_report.py research/experiments/20261123-solid-seed-main-001/zero --manifest research/datasets/append-linux-large-dev-r2/dev-linux-512.tsv --regroup-hashes research/experiments/20261123-solid-seed-main-001/regroup-hashes.json --output <new-analysis.json>
```

## 结论、限制与收尾

本轮实现并验证了“同一成熟固实策略＋Seed”的最小闭环：既有压缩帧没有被替换成较弱分块路径，默认不搜索也保留成熟策略的压缩数据，只增加明确的表示元数据。这是封装 / 解码 / 追加等价性证据，**不是搜索带来的压缩率收益**，也不是Seed独立价值的结论。

四种分组计划在小夹具验收；大规模只测默认单组、单个dev数据池、一次固定顺序，没有固实搜索器或漂移 / B-mix评估。新修订只支持off冗余，恢复与分卷明确拒绝，仍非全流式处理。下一步在相同固实目标 / 具体计划上接入有限预算搜索、旧计划复用与显式表示同算子对照，继续印度大规模实验，不回到精调旧分块D/T来证明Seed价值。

按用户本轮选择，CLion集中诊断暂缓，不作为已经通过的阶段检查；未重复请求前台。CLI / 真实构建 / 动态分析证据不冒充IDE诊断。最终源码和分析工具哈希见 final-file-hashes.json，尚未提交或推送。

收尾补充：fetch / SHA与独立分析后，已通过技能clean清理本轮远端work/tmp，保留结果 / 日志 / 源码 / 状态；本轮无运行任务。原先中止任务的unknown保护与固定工具链目录未改动。
