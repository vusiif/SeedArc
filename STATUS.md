# SeedArc 当前状态与协作交接

更新日期：2026-11-24。此文件是当前进度入口；[开发计划](docs/development-plan.md) 保存长期路线，[research](research/README.md) 保存研究证据。

接续阅读：[HANDOFF.md](HANDOFF.md)（2026-11-24交接快照，非独立进度台账；当前状态仍以本文件为准）。

用户思想统一阅读入口：[用户思想汇总](docs/user-vision.md)（2026-11-25，保留原稿及测试后方向演变，最新明确意见优先）。本次原稿两份SHA与已读快照一致；新增汇总采用联合取舍、用户选择、追加/单文件提取并列及中文支持要求，早期位宽、搜索、外置Seed和加密设想保留其候选/修正边界。仅整理文档，没有新增实验或独立审计DEV-023结果。

## 当前基线

- DEV-023 有界固实扩展dev执行完成（2026-11-25，主Agent/Claude，执行者报告通过、尚未经主模型独立审计）：阶段A复算DEV-022 0数值差异；新登记[bounded-expand-dev-r1](research/datasets/bounded-expand-dev-r1.md)（源码268.4MB / 11,285文件；文本81.6MB / 49文件，低于128MiB目标，按规则排除2个enwik8切段及含NUL的4个Silesia文件）与[预登记配置](research/configs/bounded-expand-dev-r1.json)。印度独占任务`20261125T154526Z-8e750889` exit0，496命令 / 128主编码全部verify、四单元独立审计及本机复算一致，ELF与DEV-022同SHA，CTest46/46，smoke归档Windows/Linux一致；bundle核验后已clean。源码类32/64/128MiB比整段+0.96/+0.57/+0.49%（比mixed dev大），读取 / 解码诊断约3.0×/2.2×/1.6×；文本64MiB重建−0.019%且约1.3×，128MiB与整段同SHA（给出0.87–1.61×噪声带）。追加 / 重建墙钟0.26–0.69，追加包比同档重建大源码3.7–6.7%、文本0.1–0.5%，旧原文重压0但复制整个旧包。32次故障诊断接受错误字节0，3个翻转点不足以给帧档排序。dev / 热缓存 / 单机，不设默认、不改选项表、不归因Seed；未提交推送。见[记录](research/experiments/20261125-bounded-expand-main-001/record.md) / [表](research/experiments/20261125-bounded-expand-main-001/preview.md)。

- DEV-023 测试执行任务书已准备（2026-11-24）：见[廉价模型测试计划](docs/test-execution-plan.md)。先复算DEV-022，再登记源码/文本dev分层、10%/40%新批次和四帧档平衡矩阵；只授权本任务运行器、清单与分析，不改协议或压缩实现。尚未委派、未启动远端测试。代码检查点 `d03cdaa` 已提交并推送 `origin/main`，下方各历史条目中的“未提交”描述其当时状态。用户原稿两份SHA与已读快照一致；采用最新联合取舍方向，真实落盘提取/多codec与恢复设计留待后续实现。

- DEV-022 四轮平衡复测完成（2026-11-24，主Agent）：同C++ / mixed dev四档各占一次第1/2/3/4位置，重建 / 固定第3代父包末批追加顺序交替，120命令 / 44包verify / 32提取profile通过，所有归档SHA与DEV-021一致。64MiB体积+0.050%，四轮同轮读取诊断快2.07–2.33×；32MiB+0.470%、快2.90–3.33×，改善均复现；压缩调用约11–12s，无吞吐大突破结论。32MiB追加中位3.410s vs同档重建11.906s，第4代包大4.699%，仍复制40.05MB旧包。已建立无通用默认的[研究选项表](research/configs/bounded-solid-options-r0.json)，未接正式CLI / GUI。Linux46/46、13固定包一致，ELF与DEV-021同SHA；ASan沿用同源4/4未重跑。任务`20261124T102611Z-2912f5b3` exit0、输入 / bundle及独立审计通过、已clean，无运行任务。四轮仍是同数据 / 热缓存，下一步扩数据、真实提取落盘与I/O边界，再同时间多codec比较；未提交推送。见[记录](research/experiments/20261124-bounded-repeat-main-001/record.md) / [复测表](research/experiments/20261124-bounded-repeat-main-001/preview.md)。


- DEV-021 有界固实首轮dev完成（2026-11-24，主Agent）：固定Zstd9在文件之间新增32/64/128MiB硬帧上限，已有帧表 / v4/v5语义不变；旧读者兼容、默认包字节不变。237MB mixed dev的128命令 / 40包往返 / 20对载荷、20提取 / 8损坏剖析通过；64MiB体积+0.050%，提取中位587.8→252.3ms（2.33×），32MiB+0.470%、200.0ms（2.94×）；编码11.46–12.27s，无压缩速度大突破结论。32MiB末批18.39MB追加3.35s vs重建11.73s，但第4代包比同档重建大4.70%，仍复制40.05MB旧包。损坏仅索引完好条件，非通用恢复；Seed / 显式相同收益。Windows45/45、Linux46/46、相关ASan4/4，夹具CRLF差异修复后13包跨平台SHA一致，原始证据保留。任务`20261124T092901Z-14334bcf` exit0，主 / 补查bundle及输入哈希独立核验，已clean，无运行任务；未提交推送。单遍dev不冻结预设，下一步轮换复测 / 扩数据，再处理全包I/O与多codec可比曲线。见[完整记录](research/experiments/20261124-bounded-solid-main-001/record.md)与[观测表](research/experiments/20261124-bounded-solid-main-001/preview.md)。


- 2026-11-24 用户进一步明确取舍由用户选择：优先速度、体积、损坏恢复、单文件提取或补加新文件，以少量 / 无牺牲换取明显优势。追加与单文件提取并列核心；分别计旧数据读写 / 重压量、提取额外解码量、多轮追加后的完整包大小和生命周期成本，不能隐藏定期全量重压。偏好 / 可接受代价范围及预设待实测设计，当前未实现用户选择接口，有界帧读取诊断已有dev观察（见上方），未宣称压缩吞吐或通用恢复优势。两份users_idea原稿哈希复核未变，最新聊天决定已纳入[联合计划](research/ideas/speed-resilience-r1.md)。

- 2026-11-24 用户最新主目标：接近固实压缩率时提高速度与损坏隔离 / 恢复性，或相同实际压缩时间下取得更小完整包；不能只与各算法的单项极值比较。DEV-019/020分组体积搜索收尾为机制基线，后续先成熟codec / 整段固实 / 有界分段的可比速度—大小曲线及故障恢复，再让Seed描述 / 选择 / 复用计划。规划 / 搜索和冗余成本全部计入。详见[联合计划](research/ideas/speed-resilience-r1.md)；多codec / 真实I/O主实验尚未运行，有界帧dev进度见上方。

- 2026-11-24 用户明确：当前阶段暂不考虑CLion建议性优化，集中IDE检查暂缓且不阻塞本阶段；安全错误、真实构建与往返验收仍保留。历史IDE覆盖事实不改写。

- DEV-020 异构 / 真实版本机制基线完成（主Agent，2026-11-24）：237MB / 7,993文件，正式6.12.5补丁127修改 / 7,866不变，路径投影严格核验。v0全29最佳dir2/k3，只比默认少6,209B、规划321s；v1旧计划重压反而比默认多11,552B，冷热8次同为41,507,556B / 约87–89s，默认41,512,683B / 11.56s。达到相同质量热5次 / 冷6次仅为轨迹观察，不是已实现提前停止；按用户最新速度优先标准不主推该体积搜索。25命令 / 10包往返 / 5载荷比较、Linux45/45与Windows功能夹具通过；C++ / ELF与DEV-019相同，ASan未重复（沿用同快照2/2）。任务`20261124T082849Z-65827d9a` exit0、fetch / 上传哈希及独立审计通过，已clean，无运行任务；未测恢复，无安全性优势声明，未提交推送。下一阶段见上方联合目标，CLion按用户明确授权暂缓。详见[记录](research/experiments/20261124-solid-version-main-001/record.md)。

- DEV-019 固实预算搜索 / 首轮dev完成，IDE待补（2026-11-24，主Agent）：共享Zstd-9强固实管线新增29具体计划的near-r1 / enumerate预算搜索和显式rule:k入口，旧v4/v5格式 / 语义不变。Windows44/44，印度Release45/45与相关ASan+UBSan2/2，旧包兼容、32个既有样例字节一致。相对根路径512MiB dev的25命令 / 10包往返 / 5对载荷通过；固定默认56,739,671B / 23.86s，旧计划直接追加60,711,163B / 11.13s（大7.00%），冷热搜均56,739,671B / 约85s，4次完整编码无新增收益。初始计划0使冷热起点 / 候选相同，仅测4/29，不宣称全域最优、热启价值或Seed独立优势。任务`20261124T075308Z-0ab63a9b` exit0，fetch / 源码哈希和独立原始指标审计通过，已clean，本轮无运行任务。下一步登记预算梯度、异构 / 真实版本变化dev和非默认旧计划复用；CLion按用户选择待补，尚未提交推送。详见[本轮记录](research/experiments/20261123-solid-search-main-001/record.md)。

- DEV-018 固实策略Seed最小闭环已实现，IDE待补（2026-11-23，主Agent）：新实验头部修订5 / solid-plan-r1映射2，seed-solid与explicit-solid共享既有Zstd-9固实帧编码；旧v4三模式字节 / 双向读写兼容。Windows Release43/43，印度Release与ASan+UBSan各44/44，0构建警告 / sanitizer报告；32份新修订小夹具跨Windows/Linux归档SHA完全一致。512MiB dev零搜索40命令 / 15包逐字节校验 / 10次载荷比较通过；初始和三批追加载荷与旧固实完全相同，最终新两表示均65,818,274 B，仅比旧固实多56 B元数据，regroup后多8 B。任务 `20261124T065628Z-509e5ae7` exit0，fetch SHA已独立核验，已clean临时工作区，本轮无运行任务。未实现固实搜索 / B-mix / 新修订恢复分卷，仍非全流式，不声称压缩增益；下一步在同固实管线上加有限预算搜索 / 旧计划复用 / 显式同算子对照。用户本轮暂不便前台，CLion集中诊断待补，不宣称阶段全部验收完成。详见 [执行记录](research/experiments/20261123-solid-seed-main-001/record.md) / [策略说明](research/ideas/solid-seed-r1.md)。

- 2026-11-23 用户最新研究优先级（在本轮结果之后）：利用现有成熟策略，先在固实 / 固实分段等强策略上最小接入 Seed，再重点做大规模优化与测试；小 / 简单输入不专门追求 Seed 搜索收益，可保留强默认策略和零搜索路径。已核对 v4 的 seed / solid 当前互斥，旧比较混入分块与固实布局差，不能作为“同一成熟策略上附加 Seed”的结论。下一步改为同编码路径的 Seed / 显式计划接入与零搜索验收，再进入已验收 512 MiB dev 范围；不以先制造复杂空间或开发索引优化器为前置任务。最小接入与零搜索验证现已完成，见上方DEV-018；不更改旧v4格式 / 种子语义；小文件近似等价是用户工作判断而非新增实测结论。详见 [最新定位与顺序](research/ideas/seed-append-main-r2.md#9-用户最新定位在成熟策略上使用-seed重点测大规模)。users_idea 原稿两份哈希核对未变。

- DEV-017-r2 大目录 dev 单遍成本观测完成（2026-11-23，主 Agent）：512 MiB 真实 Linux 源码、4× 预算 / seed3，9 分支 / 83 命令 / 34 逐字节校验产物的数据已独立核验。A 沿用 85.82 MB / 23.75 s；B-new 86.05 MB / 61.56 s；C 热搜重建 80.52 MB / 250.41 s；固实分段 65.82 MB / 27.42 s，固实重压 60.71 MB / 76.76 s（时间为三批搜索 + 写包调用，外部校验另列）。C 的体积减少来自清除 5.57 MB 旧尾部，实际压缩数据反而增加 0.27 MB；Seed 暖冷最终哈希及实际搜索预算均相同。本轮没有发现 Seed 独立优势，也没有覆盖真正难搜索空间（dir1 因 linux/ 前缀仅一个键）；不能推广为长期 Seed 方案的否定结论。首任务 `20261124T051435Z-cef36683` 因监控删除竞态中断，49 命令 / 20 产物已取回；续测 `20261124T054749Z-aef206e4` 在任务专用监控副本修复后 exit0，fetch SHA 核验后已 clean。全局 skill 未改，原 unknown 任务 work/tmp 保留；本轮无运行任务。结果来自跨两任务单遍 dev，非显著性 / 留出评估；下一步先补结构与固定策略对照、改进清单根路径和实际候选，再做预算梯度 / 漂移。详见 [本轮记录](research/experiments/20261123-append-large-dev-main-001/record.md) 与 [完整表](research/experiments/20261123-append-large-dev-main-001/preview.md)。

- DEV-017-r2 资源优化本轮验证完成（2026-11-23）：研究 v4 减少显式布局复制、全量解码缓存与 regroup 原文复制；不改格式、Seed 语义或输入上限。印度前后同输入 / 同归档，128 / 256 MiB 峰值 RSS 下降 14.6–14.7%。容量任务 `20261123T140842Z-51eacc6a` exit0、6 case / 48 命令逐条核验通过；512 MiB 两布局峰值 RSS 约 2.02 GiB、逻辑磁盘 1.85 GiB。此前集成任务 `20261123T134340Z-d28b6277` 触发磁盘限额 exit125，证据保留；拆出构建和历史产物后完成，未放宽资源限额。Windows Release 42/42，印度 Release / ASan+UBSan 各 43/43；旧 / 新 MSVC 的 34 个历史产物哈希一致。CLion CMake 文件无诊断，C++ 两次超时；用户人工提示核对整理后 MSVC 重建、定向 1/1 通过，最后小差异未重跑 Linux sanitizer。完整流式 I/O 仍未完成；多历史磁盘调度和真实 dev 成本测量现正接续，见上方新阶段。证据见 [资源记录](research/experiments/20261123-append-resource-main-001/record.md)。

- 2026-11-23 用户授权后续测试使用印度服务器全部资源：需要时使用 `in-long-task` 独占模式，无 CPU / 内存 / Swap / 进程数上限；同主机同时仅一个技能任务。当前固定资源前后对照不在中途变更条件。技能独占仍保留 2 GiB 磁盘预算，后续大规模须先处理磁盘工作集，不能把授权写成已执行的独占测试。

- DEV-017-r2 第一阶段已落地（2026-11-23，主 Agent）：[r2 方案](research/ideas/seed-append-main-r2.md)、代码差距与独立历史运行器已完成；MSVC / 印度 Linux 各两遍功能验收通过，每遍 34 产物 / 15 搜索，34 个跨平台产物哈希一致，含中文路径。印度任务 `20261123T130743Z-47520bcb` exit0、日志未截断，fetch 校验后已 clean，无运行任务。GNU Windows 既有工具在中文 canonical 路径处异常，两次复现，未解决；不声称全平台通过。当前目标按组可分，形式约 2690 万计划可归约到最多 987 组配置 / 203 次整份编码（仅源码成本推导，不代表实际耗时）。下一步先处理有界 I/O / 旧包复制 / 磁盘预算并测实际参考成本，不强制先扩字段，不抬高 1 GiB 上限；主性能实验尚未启动。证据及限制见 [验收记录](research/experiments/20261123-append-r2-readiness-main-001/record.md)。

- 2026-11-23 用户明确阅读优先级：判断当前研究目标时，优先查阅测试之后形成的最新 Markdown 与其中记录的用户补充，再回看初始构想；不能只检查 users_idea 两份原稿是否变化。时间顺序结合 Git 提交、正文日期与明确决定核对，不将检出时间或 Agent 本轮改写时间当成用户新想法。当前主要设计输入是 seed-append-r1 的后续追加 / 分代 / 热启动 / 部分恢复 / 分卷讨论（2026-11-15–16）；G5 记录提供此前负面结果的适用边界。新方向仍是待验证假设，不自动变成已证实收益。

- 2026-11-23 用户方向复核：Seed 的核心评估是大体积 / 难穷举的策略空间，以及接近固实压缩率下的追加、旧 Seed 复用 / 热启动与解码可复现性，不再用中小样本的小空间搜索反复判定长期价值。两份 users_idea 原稿哈希复核未变；最新定位已在 seed-append-r1 §1 / §7.5.1 有记录。本轮印度两个约 50 MB 的公开 dev 池仅作固定主机和追加流程预演，其小型计划空间不足以验证上述主张；限制已补入本轮记录，不改用户原稿，不自动重启任务。后续主实验需同时覆盖数据规模与策略空间复杂度，并先解决研究工具整批内存 / 1 GiB 上限的适用边界。

- DEV-017 印度流程预演 `20261123T123105Z-d5596074` 已于 2026-11-23 12:48:28 UTC 主动停止，不再完成剩余重复：用户最新方向需要大规模 / 难穷举主实验，本轮两个约 50 MB 的 dev 池只够预演。停止前 Release 0 警告、CTest 43/43、全部输入哈希通过；241 条完整命令记录中 0 非零退出，不能当成整轮完成。已核对 systemd 停止日志、无相关进程，586 个结果 / 日志文件已逐个 SHA256 核验取回。技能 fetch 因没有正常 result.json 拒绝打包；诊断后仅只读取回文件，未伪造完成记录。前两次失败任务均已清理；本次中止任务 work/tmp 因技能仍判 unknown 而保留，待处理该状态后再清理。详情与下一步见 [本轮记录](research/experiments/20261123-append-india-dev-main-001/record.md)。

- 2026-11-23 实验环境决定：后续正式性能实验统一放到印度 `vps-in1`，使用 `in-long-task`；固定源码、工具链、依赖、数据哈希与运行资源，环境变化后重新建立基线，详见 [追加实验计划](research/ideas/seed-append-r1.md)。本轮只用公开 linux-src / wiki-text dev，不上传本机专有二进制或读取 holdout；源码为 `479eab9`，运行器 / 配置与进度在本轮记录。本机回执在 `research/local/india-append/receipts/`。preflight 发现的两条旧任务完成状态未知，未清理或重启。

- 2026-11-12 提交整理：补充 DEV-013 已出现的工作区产物，尚未完成该研究阶段。r1 实验记录已更正 dev / test 内容重叠（614 个 test 文件）；新增 [r2 语料说明](research/datasets/b2r5-corpus-r2.md)、JSON / TSV 与生成脚本。此次实际检查：`b2_corpus_r2.py --check` 内容交集为 0，六份清单哈希与 JSON 一致，Python 语法和 JSON 解析通过。`core/codec/dictionary.*` 仅保存为未验证研究草稿，尚未纳入 CMake、编译或往返测试，不属于已完成能力。未重新运行构建、CTest、ASan 或 CLion；调试日志 `docs/debug.log` 不纳入提交。最新基线提交已为 `631cb39`（此前“未提交推送”属于历史记录）；本条对应的后续提交以 Git 记录为准。

- 最新阶段 D 第一步已完成（DEV-013，共享 zstd 字典可行性）：复核 DEV-012 中 Oracle 比 LZMA2-6 小 10,927 B（0.025 %），数字无误；r1 的 test 有 614 个文件的内容也出现在 dev，已登记为已知缺陷，原结果保留。新建 [b2r5-corpus-r2](research/datasets/b2r5-corpus-r2.md)：按项目分组划分，内容哈希不相交并有自动检查；修订 2 增加了生产路径过滤。新增研究用字典适配器 `core/codec/dictionary.*`，属 B 级、独立目标 `seedarc_codec_dictionary`，生产库与归档格式没有改动；新增研究工具 `seedarc_dict_study` 与研究容器（字典和分组表都计入字节）。dev 选参得到 D=64 KiB、训练用全部输入；test **只评估一次**：Zstd-9 加字典 2,997,893 B，比 Fixed LZMA2-6（3,563,980 B）小 15.9 %，耗时约为它的 0.57 倍，按生产索引大小折算后仍小 12.1 %，满足 H2(a)；H1（耗时不超过无字典的 1.5 倍）不成立，训练使耗时变成 1.45–3.3 倍。G5 的设计已补充（分组与字典分配），没有实现。见 [test 记录](research/experiments/20261113-dict-test-main-001/record.md)、[设计](research/ideas/shared-dictionary-r1.md)。

- 最新 B2-r5 P1 独立评估已完成（DEV-012）：adaptive-file-r1 已冻结（[配置](research/configs/adaptive-file-r1.frozen.json)，默认参数有回归测试）；新建本机独立语料 [b2r5-corpus-r1](research/datasets/b2r5-corpus-r1.md)（10 类，dev / test 各约 89 MB，按路径哈希预先划分，没有下载外部数据）。在 test 上 350/350 往返通过：Adaptive-r1 各档逐文件都不比 Zstd-3 大，但无界档（−7.45 %，14.5 s）被 Fixed LZMA2-6（−7.95 %，13.6 s）在大小和速度上同时压过；256 KiB / 1 MiB 档几乎全部耗尽预算（−0.09 % / −0.66 %）；Oracle 只比 LZMA2-6 再小 0.025 %；B-r1 seed0（有 Seed、无搜索）+26.1 %。消融 G5（有 Seed + 预算搜索）缺项，本轮不对 Seed 的独立价值下结论。见 [run 记录](research/experiments/20261112-b2-independent-main-001/record.md)、[消融设计](research/ideas/seed-ablation-r1.md)。另：其余测试夹具清理的 `remove_all` 已统一换成有界的 `tests/test_fixture.hpp`（原 seed_test_fixture.hpp 提升为通用）。

- 最新 B2-r5 P0 已完成（DEV-010）：P0-A 复现 r4 的 8 个原方案与 Oracle 共 45 组归档 SHA256 一致；P0-B 新增研究层 Adaptive-file 选择器 `adaptive-file-r1`（默认 Zstd-3、首中尾抽样、显式试编码预算、确定性回退，只输出既有 BlockPlan，归档 / 编解码 / 索引不变，metadata_bytes=0）；P0-C 统一预算对照：五组合计 Zstd-3 1,192,886 B / 60 ms，Adaptive ≥256 KiB 1,191,683 B（−1,203 B）/ 72–132 ms，Oracle 1,191,681 B / 198 ms，LZMA2-6 −320 B / 191 ms。收益全部来自 source 的一个文件；各档都不比 Zstd-3 大。195/195 往返通过，Release 36/36。仅开发样本，未调参，下一步 P1 独立语料。见 [run 记录](research/experiments/20261112-b2-adaptive-main-001/record.md)与[预登记规则](research/ideas/seed-adaptive-file-r5.md)。

- 最新 B2-r4 P0 逐块诊断与简单基线已完成（DEV-009）：text / mixed 上 B0（b-global）比固定 Zstd-3 多出的 65,594 / 66,903 B，99.78% 来自 B-r1 与内容无关地把块计划为 Store（不尝试压缩），其余为 LZMA2-6 每块多 72 B；等级、头部、索引贡献为 0，块边界一致。B1-fixed-zstd3 与 B2-file-codec 在两组均回收全部损失；source 上 B2-file-codec 8,298 B（单样本 3 配置 oracle，仅比 lzma6 小 6 B）。150/150 往返通过，8 个原方案 40 组归档与 B2-r3 逐字节同哈希；Release 全量 35/35。见 [run 记录](research/experiments/20261112-b2-diagnose-main-001/record.md)与[假设](research/ideas/seed-codec-allocation-r4.md)。未改算法、未扩大搜索、未冻结格式；Seed / 覆盖元数据仍未序列化（缺项）。

- 最新B2-r3具体计划压缩接入已验证：生产core消费具体计划，研究层解释B与覆盖；归档格式不变。Release/ASan全量各35/35、Clang定向5/5，120/120磁盘往返与双向旧包兼容通过。集中CLion9文件无ERROR，保留2条公开stop_token值传递建议。见 [验证](docs/b2-compression-validation.md)与[基线](research/experiments/20261112-b2-compression-main-001/preview.md)。仅源码样本文件覆盖减667字节，其余4类不变；未冻结Seed或宣称普遍收益。

- 最新 B2-r2 计划层契约已验证：精确路径匹配、目标删除 / 重命名后旧覆盖失效、叠加等级3→4→6→4、空文件 / 无目标 Codec 无变化、10000块单文件测试；Release34/34、ASan定向4/4、Clang定向3/3，7文件IDE无WARN/ERROR。见 [记录](docs/file-contract-validation.md)与[对照](research/experiments/20261112-file-contract-main-001/preview.md)。Seed 编码仍未冻结。

- 最新研究原型 overlay-r1 已验证：固定3文件12块，全局Zstd修改影响5块、文件1覆盖影响块5/6、清除恢复当前默认；保留A/B-r1与归档格式。Release全量33/33，定向ASan/Clang及IDE运行通过；待用户评估文件粒度。见 [本轮记录](docs/file-scope-validation.md)。

- M0 工程骨架已完成：C++23 核心库、CLI、CMake presets 与 CTest。
- M2 **P1 / P2 修复后独立复验通过，阶段完成**（代码 `4c547b2`）：主线程重建并执行 MSVC Release 25/25、ASan Release 25/25、CLion Debug 定向验收 exitCode=0；四个修复相关文件 IDE 诊断完整返回无 ERROR。原实现者七套配置证据保留。发布树保留已确认的私有 DACL，受信同 SID / SYSTEM / 管理员在最终复核后修改不在保证内；格式仍实验性、未冻结。见 [验收记录](docs/m2-acceptance.md)“第二轮独立复验”。
- M3 **独立验收通过，阶段完成**（DEV-004 / ACC-003，2026-11-12）：按块 Zstandard / LZMA2 编码与 Store 回退，依赖由 vcpkg manifest 钉住版本与 baseline；本轮独立 Release / ASan 各 30/30、CLion 13/14 文件完整返回无 ERROR（另 1 文件由用户人工补查仅拼写提示）；随后修复默认 Debug profile，IDE CLI 与 codec 测试均成功；原实现者七套配置（含 ASan）30/30，Fuzz 短跑无崩溃；CLion 诊断（clang-ucrt64-debug，errorsOnly=false）覆盖 13 个改动文件，无 ERROR，修复 2 条有效 WARN 后三套配置复测 30/30。见 [M3 记录](docs/m3-validation.md)。
- M1 已完成并由用户验收：实验头部 / 块范围 / 路径校验和安全 I/O 原语，含卷身份、累计深度 / 路径预算、共享模式调整与故障注入测试；头部 / 路径 libFuzzer 短跑无崩溃。六套配置与 MSVC ASan 均 18/18、无警告；CLion 诊断已完成（修复 1 个 ERROR）。未接 CLI，见 [接续记录](docs/m1-validation.md)与[候选 ADR](docs/decisions/0001-experimental-container-and-output.md)。
- 已有项目规范、开发路线、GPL-3.0-or-later 声明及正文，Git 远端 origin 为 https://github.com/vusiif/SeedArc（main 已推送首次提交 `fbd3fb8`）。
- 已完成 M4-A 第一轮独立研究原型：A/B-r1 输出计划与字段差异，尚未接入归档、尚未由用户评估语义。小型 M3 合成基线 27/27 内容哈希往返通过；Release / ASan 各 32/32，Clang 定向 2/2，6 文件 CLion 完整诊断无 WARN / ERROR。见 [M4-A 记录](docs/m4a-validation.md)；没有 Seed 搜索收益或独创性结论。
- third_party 已保存有来源许可与适配记录的 SplitMix64 单步片段，仅用于种子研究；轻量 Qt GUI 改为按用户人工交互测试需要启动；Actions 按用户最新要求延后。

## 里程碑

| ID | 目标 | 状态 | 完成门槛 / 证据 |
| --- | --- | --- | --- |
| M0 | C++23 / CMake / CLI / CTest 骨架 | 完成 | 三套工具链 Debug / Release 各 13/13，另有 MSVC ASan 13/13；见验证记录 |
| M1 | 实验格式与安全 I/O | 完成（用户 2026-11-11 验收） | 六套配置 + ASan 18/18，故障注入与 Fuzz 短跑通过，CLion 诊断无剩余 ERROR；已知限制：真实 SUBST 未测、Fuzz 仅短跑；见 M1 记录 |
| M2 | Store 归档往返 | 完成（2026-11-11 独立复验通过） | 代码 4c547b2；独立 Release / ASan 各 25/25，IDE Debug 定向回归通过；保留已确认 DACL 与信任边界，见 [验收记录](docs/m2-acceptance.md) |
| M3 | Zstd / LZMA2 | 完成（2026-11-12 独立验收） | 本轮 Release / ASan 各 30/30、IDE 13/14 完整诊断无 ERROR，限制见 M3 记录；原实现者 七套配置 + ASan 30/30，archive Fuzz 180 s 无崩溃；CLion 诊断无 ERROR、2 条 WARN 已修；见 [M3 记录](docs/m3-validation.md)、[ADR-0003](docs/decisions/0003-m3-zstd-lzma2-codecs.md) |
| M4 | 种子设计探索、确定性与 MVP | B2-r3真实压缩实验已验证，待用户评估与继续选型 | 先比较候选并由用户评估语义，再选择实验方案接入；固定向量、归档复现、动态检查与基线 |
| UI-1 | 按人工交互测试需要启动的轻量 Qt GUI | 延后，按需启动 | 基础操作、往返与错误显示、界面响应，核心无 Qt 依赖 |
| M5 | 公平基准与预算搜索 | 待开始 | 可复现实验、取消与最优结果保留 |
| UI-2 | M5 后的搜索界面 | 待开始 | 预算、真实搜索曲线与结果对比 |
| M6 | 结构化研究与产品化 | 待开始 | 按实验收益及发布门槛逐项推进 |

## 任务表

状态：待开始 / 进行中 / 待验收 / 完成 / 阻塞。领取时填写真实负责人和写入范围；完成后保留精简证据。

| 任务 ID | 任务 | 负责人 | 写入范围 | 状态 | 验收 / 证据 |
| --- | --- | --- | --- | --- | --- |
| DEV-023 | 有界固实扩展dev与批次取舍测试 | 主Agent（2026-11-25领取，Claude） | 本任务Python运行器 / 配置 / 清单 / 实验记录与分析；`research/experiments/20261125-bounded-expand-main-001/` | 执行者报告通过，待主模型审计 | [记录](research/experiments/20261125-bounded-expand-main-001/record.md)：阶段A复算0差异；496命令 / 4单元独立审计通过，128主编码全verify，ELF与DEV-022同SHA，CTest46/46；任务`20261125T154526Z-8e750889` exit0、bundle核验、已clean；未改协议与压缩实现 |
| DEV-022 | 有界固实平衡顺序稳定性复测 | 主Agent | 轮换运行器 / 分析 / 研究选项与记录 | 复测完成，研究选项已列出 | 四轮120命令 / 44包SHA一致，32/64MiB读取收益均复现；同数据 / 热缓存，下一步扩数据 / I/O，见记录 |
| DEV-021 | 有界固实分段联合取舍dev | 主Agent | append4私有写入选项 / 计量、测试、研究脚本 / 文档 | 首轮dev完成 | 128命令 / 40包 / 20对载荷，Windows45、Linux46、相关ASan4；小体积代价换提取改善，单遍待轮换复测，见完整记录 |
| DEV-020 | 异构与正式版本变化策略复用dev | 主Agent | 新数据清单 / Python运行器 / 分析 / 研究文档 | 完成机制基线，转速度 / 恢复目标 | 25命令 / 10包 / 5对载荷、Linux45/45；237MB正式版本127修改，按速度优先不主推87s只省5KB；详见DEV-020记录 |
| DEV-019 | 固实预算搜索、旧计划复用与公平对照 | 主Agent | solid_plan.hpp、append4_study.cpp、CMake / 研究测试、新清单 / 运行器 / 分析 / 文档 | 实现与本轮实验完成，IDE待补 | Windows44/44、印度Release45/45、相关ASan2/2；25命令 / 10包 / 5载荷对照，4×搜索无收益，追加大7%但更快；见当前基线与DEV-019记录 |
| DOC-001 | 研究归档及协作规范 | 本次主 Agent | AGENTS、STATUS、README、研究目录及相关流程文档 | 完成 | 本地链接、JSON 模板与归档结构检查通过；仅文档工作 |
| DOC-002 | 第三方源码目录与 UI 计划同步 | 本次主 Agent | third_party/README、AGENTS、README、STATUS、开发计划与架构文档 | 完成 | 21 份 Markdown 本地链接检查通过，third_party 文档未被项目规则忽略；无源码引入或构建测试 |
| DOC-003 | 用户想法更新检查与设计输入 | 主 Agent | AGENTS、STATUS、README、开发计划与流程文档 | 完成 | 已读两份原稿，记录哈希与影响；23 份 Markdown 本地链接及原稿哈希核对通过，原稿未改动；仅文档工作 |
| IDE-001 | CLion MCP 诊断接入与规范 | 主 Agent | AGENTS、开发说明、STATUS，必要的相关代码修正 | 完成 | 超时根因已定位（Nova 只分析可见编辑器；窗口失焦后不出结果），方法写入开发说明；17 个源文件 / 头文件取得 errorsOnly=false 诊断，修复 1 个 ERROR（越界枚举转换），其余仅风格提示；见 [M1 记录](docs/m1-validation.md#clion-诊断第四轮) |
| DEV-001 | M0 工程骨架 | 主 Agent | CMake 配置、core、cli、tests、构建说明与 STATUS | 完成 | 六套配置各 13/13，ASan 13/13、格式及静态检查通过；见 [M0 记录](docs/m0-validation.md) |
| REV-001 | 外部审查阶段 1 修复 | 主 Agent | README、开发计划、验证记录、ADR、CLI 提示、error.hpp、archive.hpp、safe_output、相关测试、ADR-0002 | 待验收 | 审查原文 [docs/review_results/](docs/review_results/SeedArc_项目代码审查.md) 未改动。采纳：状态统一到 STATUS；CLI 提示去里程碑化；新增 `ErrorCode::cancelled`；`max_blocks` 1000000→262144 并加 `limits_are_coherent` 与边界测试；M2 约束写入 [ADR-0002](docs/decisions/0002-m2-store-archive-constraints.md)。延后：流式写出 / 事务 / 完整索引（M2）、CI（需用户决定，Linux 需先定 POSIX 路径规则）、CRC32C 加速（先测量）。六套 + ASan 18/18、format / tidy 通过；CLion（前台、逐个打开）复查 6 个改动文件无 ERROR，仅风格提示；safe_output.cpp 两条 `Clang-Tidy: Potential memory leak`（第 248 行 `new (storage.data()) FILE_RENAME_INFO{}` 及其在析构处的报告点）判为误报：placement new 不分配堆内存，对象可平凡析构，存储归 `std::vector` 所有；未压制检查 |
| DEV-003 | M2 Store 归档端到端 | 原实现 / 修复会话；本轮主 Agent 独立验收 | core、cli、tests、fuzz、ADR-0002、M2 记录、README、STATUS | 完成 | P1 / P2 独立复验通过：Release / ASan 各 25/25，IDE Debug 定向回归成功，4 文件完整诊断无 ERROR；本轮未改实现、未提交推送；[验收记录](docs/m2-acceptance.md) |
| DEV-013 | 跨文件共享字典可行性（阶段 D）、r2 分组语料、G5 设计 | 主 Agent（B2 协调会话委派） | core/codec/dictionary.*、core/CMakeLists.txt（一个研究目标）、tests/codec_dictionary_tests.cpp 与 tests/CMakeLists.txt、research/tools/dict_study.cpp 与 CMakeLists、research/scripts/b2_corpus_r2.py / b2_dictionary.py / README、research/datasets/b2r5-corpus-r1*（缺陷登记）/ r2*、research/ideas/shared-dictionary-r1.md / seed-ablation-r1.md、runs 20261113-dict-*、20261112-b2-independent-main-001/record.md（更正）、STATUS | 完成，待用户评估 | dev 252 次 + test 90 次往返全部一致，同组产物相同；5 种损坏或截断的容器都被拒绝；`codec.dictionary_research` 通过；MSVC Release / ASan、GCC Debug / Release、Clang Debug / Release 全量 CTest 各 37/37（新增 codec.dictionary_research），构建 0 警告，clang-format 通过，Clang Debug 下研究工具 `seedarc_dict_study` 也编译通过；CLion 阶段诊断（2026-11-14，`errorsOnly=false`）：没有 ERROR，删掉了 `dict_study.cpp` 中未使用的 `<map>`，保留的 WARN 及理由见 [L2 / L3 记录 §8.4](docs/linux-l2l3-validation.md)。合并后 `fbeffb4` 两个平台的全量矩阵见同一记录 §8 |
| DEV-012 | 冻结 r1、独立语料、B2-r5 P1 评估、Seed 消融设计、夹具清理统一 | 主 Agent（B2 协调会话委派） | research/configs/adaptive-file-r1.frozen.json、research/datasets/b2r5-corpus-r1*、research/scripts/b2_corpus.py / b2_independent.py 与 README、research/ideas/seed-ablation-r1.md、seed-adaptive-file-r5.md、新 run 20261112-b2-independent-main-001、tests/test_fixture.hpp（由 seed_test_fixture.hpp 改名并扩充）、tests/{seed_adaptive,seed_archive,store_archive,codec,store_acceptance}_tests.cpp、STATUS | 完成，待用户评估 | 350/350 往返、同组哈希一致；MSVC Release / ASan、GCC Debug / Release、Clang Debug / Release 全量各 36/36，构建 0 警告；GCC Debug 与 Clang Debug 下改动的 5 个测试各重复 50 次，0 失败、0 超时、无残留夹具；CLion 6/6 文件 0 ERROR / 0 WARN（store_archive_tests.cpp 在前台补查） |
| DEV-011 | `seed.archive_integration` 偶发失败排查 | 主 Agent（B2 协调会话委派，用户同意） | tests/seed_archive_tests.cpp、tests/seed_adaptive_tests.cpp、新 tests/seed_test_fixture.hpp、docs/b2-compression-validation.md、STATUS | 完成 | 根因：外部组件在删除后短暂列出的“原名大写 + .tmp”条目干扰了测试计数，另有 libstdc++ remove_all 死循环；实现无残留、无句柄泄漏。Release/ASan 全量 36/36；重复运行 Release 1000、ASan 200、Clang 1500 次 0 失败；见验证记录 DEV-011 节 |
| DEV-010 | B2-r5 P0 可复现基线、Adaptive-file 与统一预算对照 | 主 Agent（B2 协调会话委派） | core/seed/adaptive_file.*、core/CMakeLists 一行、research/tools/seed_archive.cpp（r5 输出、adaptive-file / oracle-file）、tests/seed_adaptive_tests.cpp 与 tests/CMakeLists、research/scripts/b2_adaptive.py 与 README、research/ideas/seed-adaptive-file-r5.md、新 run 20261112-b2-adaptive-main-001、STATUS | 完成 P0，待用户评估 | Release 36/36；195/195 往返，P0-A 45 组同哈希；ASan 36/36，Clang 定向通过（修 1 条有效警告后 0 警告），最终二进制复核 35 例一致；CLion 6 文件 0 ERROR，2 条 WARN 已修；既有 seed.archive_integration 偶发失败已由 DEV-011 修复 |
| DEV-009 | B2-r4 P0 逐块诊断与简单分配基线 | 主 Agent（B2 协调会话委派） | research/tools/seed_archive.cpp（r4 输出与 file-plan 模式）、research/scripts/b2_diagnose.py 与 README、research/ideas/seed-codec-allocation-r4.md、新 run 20261112-b2-diagnose-main-001、STATUS | 完成 P0，待用户评估 | Release 35/35；150/150 往返、对账 50/50、块边界一致、原方案 40 组归档与 B2-r3 同哈希；ASan 35/35，Clang 定向构建 0 警告；file-plan 负面 11 项；格式化后复核；CLion seed_archive.cpp（errorsOnly=false）0 ERROR / 0 WARN，仅 INFO |
| DEV-008 | B2-r3 具体计划压缩接入与小型基线 | 主 Agent | core内部计划入口、seed研究适配、工具 / 测试 / CMake、规则 / 新实验与状态 | 集成与小型基线通过，待用户评估 | Release/ASan各35/35、Clang5/5，修正后定向回归通过；120次磁盘往返、双向兼容、5例跨工具链一致；9文件CLion无ERROR，限制见 [记录](docs/b2-compression-validation.md) |
| DEV-007 | B2-r2 文件身份与叠加契约 | 主 Agent | core/seed/path_overrides、测试 / 研究工具 / 规则与新实验、相关 CMake、STATUS | 集成验证通过，待用户评估 | Release34/34、最终定向4/4；ASan定向4/4及最终新测试1/1，Clang3/3；7文件IDE无WARN/ERROR，IDE测试与工具退出0；见 [记录](docs/file-contract-validation.md) |
| DEV-006 | 文件级等级覆盖最小原型 | 主 Agent；子任务 m4_seed_prototype | core/seed/file_overrides、研究工具、相关 CMake / 测试、规则与本轮实验 / 验证记录 | 集成验证通过，待用户评估 | Release 33/33，IDE 修正后定向3/3、ASan定向3/3、Clang定向2/2；4组跨编译器输出一致，4文件IDE无ERROR（保留1条已核实include提示）；不改A/B或归档，见 [验证记录](docs/file-scope-validation.md) |
| DEV-005 | M4-A 种子候选原型与小型 M3 基线 | 主 Agent；子任务 m4_seed_prototype | core/seed、research/tools、相关 CMake / 测试、third_party/splitmix64；research 配置 / 脚本 / 本轮实验 / 文档 | 原型通过，待用户评估 | 主线程 Release / ASan 各 32/32、Clang 定向 2/2，跨工具链 4 组输出字节一致；6 文件完整 IDE 诊断无 WARN / ERROR，IDE 两个目标运行成功；基线 27/27，通过范围与限制见 [M4-A 记录](docs/m4a-validation.md) |
| PLAN-004 | Seed 探索路线与优先级调整 | 主 Agent | 开发计划、架构、research/ideas、STATUS | 完成（仅设计记录） | 位宽 / 字段 / 映射未冻结；新增表示与映射替换边界、候选修订及旧包解码约束，加入 Agent 规范；A/B/C 候选与用户评估关口，GUI 按需、Actions 延后；未实施算法或运行实验，见 [设计探索](research/ideas/seed-design-study.md) |
| IDE-002 | CLion 默认 Debug 依赖配置修复 | 主 Agent | 本地 CLion Debug profile、开发说明、M3 记录、STATUS | 完成 | MCP 重新加载成功；确认 MSVC / C++23 模式与 vcpkg 依赖；IDE CLI 版本、codec 测试均 exitCode=0，CMake 文件完整诊断无问题，见 M3 记录末节 |
| ACC-003 | M3 独立验收 | 本轮主 Agent；只读子审查 m3_review | STATUS、M3 验收记录；不改原实现 | 完成 | Release / ASan 各 30/30；format / diff 检查通过，IDE 13/14 文件完整无 ERROR；1 文件人工补查仅拼写提示；IDE 运行已在 IDE-002 补齐，详见 [M3 记录](docs/m3-validation.md#独立验收2026-11-12) |
| DEV-004 | M3 Zstd / LZMA2 块编码 | 主 Agent | vcpkg manifest、CMakePresets、core/codec、archive / store_archive / safe_output、CLI、tests、fuzz 语料、ADR-0003、M3 记录、README、开发说明、STATUS | 完成（独立验收通过） | zstd 1.5.7 / liblzma 5.8.4 经 vcpkg 钉住；七套配置 + ASan 30/30，format / tidy 通过，archive Fuzz 180 s 无崩溃；CLion 诊断 13 个文件无 ERROR，修 2 条 WARN 后 MSVC Release / Clang Debug / GCC Release 复测 30/30；[M3 记录](docs/m3-validation.md) |
| DEV-014 | Linux L1 + L2 / L3：WSL 构建与 presets；POSIX 输入枚举与安全输出（ADR-0004）；测试移植到与 Windows 同广度 | DEV-014 子会话（协调会话委派） | WSL 克隆分支 `linux-l1`（L1）与 `linux-l2l3`（L2 / L3）：CMakePresets、根 / core / cli / tests / fuzz CMake、core/filesystem/{archive_path,safe_output,input_tree}.cpp 与新 *_posix.cpp / posix_common.hpp、safe_output.hpp、cli/main.cpp、tests 下 FS 测试与 test_fixture、新 safe_output_posix_tests.cpp、check_cli.cmake、ADR-0004、docs/development.md §7、linux-l1 / linux-l2l3 验证记录、posix 草案、STATUS | 已合并到 main（`fbeffb4`，已推送），待用户验收 | L2 / L3：GCC / Clang Debug / Release 与 ASan+UBSan 各 37/37（36 项与 Windows 同名 + `cli.posix_refusals`，无 DISABLED），全量重编 0 警告；FS 九项各重复 20 次 0 失败；fuzz 三目标各 300 s 0 崩溃；内存有界在上限内（RSS 口径）。Windows（3f8d84c，协调会话在独立 worktree 中验证）：MSVC / UCRT64 GCC / UCRT64 Clang 的 Debug / Release 与 MSVC ASan 各 36/36，0 警告，无 ASan 报告；合并后 `fbeffb4`（2026-11-14）：全量矩阵 Windows 7 套各 37/37、Linux 5 套各 38/38，0 警告，没有 sanitizer 报告；msvc-fuzz 与 clang-linux-fuzz 三个目标各 300 s，0 崩溃；两个平台各 20 次重复测试，0 失败；xfs / btrfs / tmpfs / ext4 都是 12/12；印度服务器内核 6.8 上 12/12，strace 证实走了 `/proc/self/fd` 发布回退；CLion 诊断没有 ERROR（POSIX 源文件在 Windows IDE 中没有覆盖）。非 ASCII 名字按用户选择暂不支持。见 [L2 / L3 记录](docs/linux-l2l3-validation.md)、[L1 记录](docs/linux-l1-validation.md) |
| DEV-015 | 跨平台 Unicode 路径规则（[ADR-0005](docs/decisions/0005-portable-unicode-path-profile.md)）：Linux 支持 Unicode 名字；Windows 不再调用 IsNormalizedString / CompareStringOrdinal | 协调会话 | core/filesystem/{unicode_profile.*,unicode_tables_gen.py,unicode_tables.inc,archive_path.cpp,safe_output_posix.cpp}、archive_path.hpp、core/CMakeLists.txt、tests（unicode_tests.cpp 及路径 / store / codec / seed_path / check_cli 用例）、research/tools/unicode_win_diff.cpp、third_party/unicode-ucd/、.gitattributes、ADR-0001 / 0004 / 0005、[验证记录](docs/unicode-path-validation.md)、STATUS | 完成，待用户验收（未提交）。中途曾按用户要求暂停，当天恢复 | 最终代码：Windows 7 套各 39/39、Linux 5 套各 40/40，0 警告，没有 sanitizer 报告；NormalizationTest 全量一致；`unicode.tables_current` 为最新；msvc-fuzz 三个目标各 300 s、Linux path / archive fuzz 各 300 s，0 崩溃；两个平台各 12 项 × 20 轮重复测试，全部通过；Win32 差分显示新规则只更严。CLion 补查四个文件，没有 ERROR，WARN 及理由见验证记录 §3。**未解决**：暂停前出现过一次 Linux `.sandbox-test` 残留，之后未再出现，原因不明。见[验证记录](docs/unicode-path-validation.md) |
| DEV-016 | G5：带预算搜索的结构化 Seed（分组与字典分配），新留出集 `g5-holdout-r1`（本机 + 公开混合） | 协调会话 | research/ideas/seed-g5-r1.md、research/tools（dict_study 扩展与路径检查工具）、research/scripts（建库与分析）、research/datasets/g5-holdout-r1*、research/configs、research/experiments/2026111x-g5-*、research/local/g5-holdout-r1（被忽略）、STATUS | 完成，待用户评估：留出集 [g5-holdout-r1](research/datasets/g5-holdout-r1.md)（2,679 个文件，11.4 MB，与 r2 内容交集 0）已建并评估一次；`seedarc_g5_study`（研究容器 v2，random / coord / seed 三种搜索）已实现；结果为负面（H2 不成立） | 测试 `research.g5_study`（确定性、往返、Seed 只在 seed 方法中出现、116 个损坏样本全拒、10,000 个 Seed 往返）在 MSVC Release、GCC UCRT64、Linux GCC、Linux ASan+UBSan 上都通过；`seedarc_path_check` 在两个平台上判断一致；dict_study 改用共享头文件后输出逐字节不变；主测试 MSVC Release 39/39。dev 探索 [001](research/experiments/20261114-g5-dev-main-001/record.md) 和确认扫描 [002](research/experiments/20261114-g5-dev-main-002/record.md)（180 次搜索，全部逐字节往返）：小预算下方法之间有差别；docs 4× 时 seed 最好，config 4× 时 coord 最好；8× 起 coord 和 seed 打平，收益 0 到 −2.6 %（dev，不作结论）。配置已冻结为 [g5-r1](research/configs/g5-r1.frozen.json)（随 `362fb81` 推送），假设没有根据 dev 结果修改。**留出集正式评估（唯一一次）[20261115-g5-holdout-main-001](research/experiments/20261115-g5-holdout-main-001/record.md)：H0 成立（549 个产物逐字节往返，18 格重跑结果相同），H1 不成立，H2 不成立，即没有发现 Seed 的独立价值（负面结果）。** 分组搜索相对单个全局字典只多 0 到 −1.5 %；单个全局字典相对 LZMA2-6 的优势在新数据上复现（直接 −11 % 到 −35 %，折算 −8 % 到 −25 %）。分析由 `g5_analyze.py` 从原始数据生成；评估结果未提交 |
| DEV-002 | M1 实验格式与安全 I/O 设计 | 主 Agent；两个已结束子任务 | 候选 ADR、archive / filesystem 模块、tests、验证记录 | 完成 | 用户 2026-11-11 验收。error 5（沙盒 ACL）定位、提交重命名修复；卷身份 / 深度预算 / 共享模式 / 故障注入 / Fuzz 完成；六套 + ASan 18/18，format / tidy 通过；CLion 诊断完成并修 1 个 ERROR；真实 SUBST 未测；[M1 交接](docs/m1-validation.md) |

## 验证与证据

- 文档及模板检查：本地 Markdown 链接目标存在、JSON 模板可解析、研究归档目录齐备。
- M0：MSVC 19.51、GCC 16.2、Clang 22.1 的 Debug / Release 编译、链接、运行及 CTest 全部通过，每套 13 项；MSVC Release ASan 13/13，clang-format 与 clang-tidy 的分析器检查通过。命令、范围和日志位置见 [M0 验证记录](docs/m0-validation.md)。
- M2（2026-11-11）：七套配置全量重建后 CTest 24/24、构建 0 警告、MSVC ASan 无报告；`clang-format` / `clang-tidy` 通过；64 MiB 单文件往返峰值工作集增长约 1.4 MiB（断言上界 16 MiB）；Fuzz 短跑结果、命令、日志位置和未验证项见 [M2 记录](docs/m2-validation.md)。本轮未取得 CLion 诊断（clion MCP 服务 ECONNREFUSED）。
- M4-A：研究运行包括 20261112-m3-baseline-main-001、20261112-seed-plans-main-001，以及文件覆盖观察 20261112-file-scope-main-001、行为契约观察 20261112-file-contract-main-001、真实压缩首轮 20261112-b2-compression-main-001；前者为小型单块合成基线，后者仅观察拟议计划。已运行小型磁盘源码/自制样本压缩基线；尚未运行独立代表性真实语料 / Seed搜索评估、持续Fuzz。
- 本轮 DEV-008 由主 Agent 实现与验证，未启动新子任务；前轮子任务 m4_seed_prototype 已交还写权限；无运行中的构建 / 研究任务。实验事实与源码快照见上述 run ID。
- CLion MCP：通过客户端已有配置发现回环服务（不硬编码连接地址），initialize 确认为 CLion MCP Server 2026.3.0.1；实际调用 tools/list、get_run_configurations、get_compiler_info、open_file_in_editor、get_all_open_file_paths 和 get_file_problems。确认 SeedArc 项目、MSVC Debug、`-std:c++latest` 和 `/Zc:__cplusplus`；活动编辑器为 cli/main.cpp。当前会话无原生 JetBrains 工具入口，使用已配置服务的实际 MCP JSON-RPC 调用，不以命令行分析冒充 IDE。
- M1 第三轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 18/18（新增 fuzz 语料重放 2 项）；`msvc-fuzz` libFuzzer header 3065144 次 / path 576342 次（各 151 s）无崩溃；format / tidy（13 个 .cpp）通过；CLion 仍 timedOut。用户想法 SHA256 复核未变。
- M1 第二轮（2026-11-11）：六套 presets 与 MSVC ASan 构建 0 警告、CTest 16/16；clang-format / clang-tidy（全部 10 个 .cpp）通过；CLion get_file_problems 对 4 个文件仍 timedOut。详见 [M1 记录](docs/m1-validation.md)。用户想法两份 SHA256 复核未变。
- CLion 检查：早期超时的根因已在 IDE-001 定位（只分析可见编辑器、窗口需在前台），方法见[开发说明](docs/development.md#clion-mcp-检查)。返回 `timedOut=true` 的空结果不代表没有诊断。

## 阻塞、限制与下一步

- DEV-013 验证情况：已运行 dev 调参（run 001 因路径问题失败，记录保留；run 002 共 252 次）、test 正式评估一次（90 次），每次都逐字节往返，同组产物一致；5 种损坏或截断的容器都被拒绝；6 套配置全量 CTest 37/37。CLion 阶段诊断已于 2026-11-14 补做（见 [L2 / L3 记录 §8.4](docs/linux-l2l3-validation.md)）；合并后 `codec.dictionary_research` 在 Linux GCC / Clang / ASan+UBSan 下也通过。**没有验证的**：针对字典适配器的专门 Fuzz；字典实验在非 Windows 平台上的重跑；冷缓存与多机复现；研究容器的流式写出和原子提交（本来就没有实现）。实验结论只适用于 r2 test 的三类文本数据（11.6 MB / 2,294 个文件），r2 test 已用过一次。

- DEV-013 后续（尚未实施）：(1) G5 按 seed-ablation r2 的设计实现时，只能在 r2 dev 上开发，评估需要新的留出集（r2 test 已用过一次）；(2) 固实压缩作为对照方向；(3) 若要让字典进入生产路径，需先做格式设计并写 ADR（C 级，涉及格式），并改为流式写出和原子提交；(4) 研究工具目前把整个输入读进内存，压缩内存数字不能代表流式实现。Linux 移植（DEV-014）由另一会话在 WSL 的 `linux-l1` 分支进行，`dictionary.cpp` 和 `dict_study.cpp` 都没有新增 Windows 专用代码（计时与内存在 `_WIN32` 之外改用 getrusage）。

- DEV-012 下一步建议（尚未实施）：在 **dev** 划分上做 adaptive-file-r2：把 LZMA2-6 纳入默认或候选顺序、重新审视 16 KiB 小文件阈值（terminfo 上 LZMA2 能省 11 %，r1 为 0）、用廉价特征跳过已压缩数据（compressed-packages 试编码 8.6 MB 一字节未省）、改按时间计预算，最后再在 test 上做一次性评估。G5 搜索器、跨文件字典 / 固实压缩（P1-1）仍待立项。CLion 对 tests/store_archive_tests.cpp 的诊断已在前台补查，只有 INFO。本轮未提交推送。

- DEV-011 已排查并修复 `seed.archive_integration` 偶发失败（DEV-010 发现）：**测试观测问题，不是实现问题**。在同一次遍历中计数并记名的诊断下，Release 300 次失败 22 次，全部是外部组件在文件删除约 0.7 ms 后短暂列出、随后约 0.2 ms 消失的“原名大写 + .tmp”条目（`FIXED-*.SARC.tmp`）。这些条目打开即 NOT_FOUND，与被删对象的 FileId 不同；所删的是测试自己刚用 `fs::remove` 删掉的已提交对照归档，失败写入从未残留。具体是哪个外部组件未查明（Defender 未运行；Windhawk 已注入但只有时钟 mod；`fltmc` 无权限）。验证过程中另发现 libstdc++ 16.2 的 `remove_all` 遇到同类正在消失的条目时死循环（Clang 下 CTest 超时；gdb 栈为 `remove_all→__erase→remove→symlink_status`，卡住的路径是失败写入临时名对应的外部条目；外部可 rmdir，证明没有泄漏句柄）。改动只涉及测试：失败写入改到专用目录并按对象核对（删除挂起仍算残留，并有自检）；清理改用有界的 `seed_test::remove_tree`；安全扫描对已消失的名字视为安全。验证：Release / ASan 全量各 36/36；`seed.archive_integration` 在 Release 200+300 次、ASan 200 次、Clang 200+300 次 CTest 重复中 0 失败，另有直接运行 Release 500 次、Clang 2×500 次 0 失败 / 0 卡死 / 0 残留；CLion 3 个测试文件 0 ERROR，新增 1 条 WARN 已修。证据见 `build/dev011/` 与 [验证记录](docs/b2-compression-validation.md#dev-011seedarchive_integration-偶发失败排查2026-11-12)。后续：`store_archive_tests`、`codec_tests`、`store_acceptance_tests` 的夹具清理仍用 `remove_all`，在 GNU 工具链下理论上同样可能卡死，未改。

- B2-r5 下一步（P1，未做）：引入独立授权语料，预先划分开发集与评估集，在评估集上一次性检验 `adaptive-file-r1`；不在现有 5 组样本上调阈值。本轮未提交推送。

- DEV-009（B2-r4 P0）已解释 text / mixed 差距：主因是 B-r1 把约 1/3 块计划成 Store。建议候选（未实施，需用户评估）：B 的 Codec 字段不再把 Store 作为可计划项、交给 core 回退；后续 Seed 对照至少包含 B1-fixed-zstd3 与 B2-file-codec 基线并计入试编码成本与元数据。下一步按评估计划 P1 扩充代表性授权语料。本轮未提交推送。

- DEV-008已完成B2-r3真实压缩首轮。下一步评估小型基线并选择更有代表性的授权数据，再设计公平预算的候选/搜索对照；固定Codec分配中的Store占比代价需要考虑。最终Seed编码、身份和映射仍可探索。当前B2-r1/r2/r3与文档规则改动均未提交推送；用户原稿与旧实验未改写。

- 2026-11-12 用户最新方向：优先种子设计，先比较候选、让用户测试语义，再选择实验方案；不提前冻结位宽 / 字段 / PRNG；后续持续探索，通过表示与映射解耦、候选修订和旧数据兼容保留修改空间，不预建插件系统。设计输入见 [Seed 设计探索](research/ideas/seed-design-study.md)。GUI 仅在交互测试需要时启动；Actions 延后，本轮未创建工作流。M4-A与B2-r3小型真实压缩实验已完成；下一步评估基线及更有代表性的语料，再决定搜索对照，不自动冻结协议。

- CLion 旧手工 Debug profile 缺 vcpkg 工具链问题已修复（IDE-002）：当前使用 `cmake-build-debug-vcpkg`，MCP 重新加载、IDE CLI 和 codec 测试均成功；旧目录保留。用户人工补查最后一个旧测试文件仅拼写提示，忽略。历史 IDE 运行失败已由本次成功验证补齐，详见 M3 记录末节。

- M3 2026-11-12 独立验收通过：独立验收阶段只改文档，随后修复 CLion 本地配置；Release / ASan 各 30/30，IDE 13 文件完整诊断、1 文件人工补查仅拼写提示，IDE CLI / codec 运行已补齐。M3 实现及记录按用户授权提交交付，限制见 M3 记录。用户想法两份哈希未变；下一步 M4。

- M0 已核对实际工具链并完成验证；GNU presets 要求当前终端设置 SEEDARC_UCRT64_ROOT，ASan 运行时路径按实际 VC Tools 安装位置设置，详见构建说明。未验证 Windows 之外平台。
- 之前 Gopeed MCP 不可用；许可证正文已从本机标准副本取得，此问题不阻塞工程骨架。
- Git 默认 Schannel 访问曾失败，单次 OpenSSL 查询成功；沙盒读取用户全局 ignore 文件受限，项目内规则已检查。未改系统或全局 Git 配置。
- M1 已由用户验收（已知限制：真实 SUBST 盘符未测、Fuzz 仅短跑），首次提交 `fbd3fb8` 已推送到 origin/main。外部审查阶段 1 修复（REV-001）已提交为 `4c25a27`（本地 main 与 origin/main 的跟踪引用一致，未联网复核），任务仍待用户验收。
- M2（DEV-003）P1 / P2 已在代码 `4c547b2` 修复并独立复验通过；本轮仅更新验收 / 状态文档，未提交或推送。私有 DACL 行为已确认；ASan 需 VC Tools `bin\Hostx64\x64` 在进程 PATH。用户想法两份 SHA256 未变，测试无残留，无后台任务；下一步为 M4；M3 独立验收见 ACC-003，本轮不启动新阶段。
- **M2 独立验收历史**：首轮 Release 24/24，但新增回归在 Release / CLion Debug 复现两项缺陷而未通过；本轮修复后已通过上述独立复验。历史失败、修复、最新证据分别保留在 [验收记录](docs/m2-acceptance.md)，不删除负面证据。需要 IDE 前台时先提醒用户配合。
- Git 仓库属主为沙盒账户，当前用户执行 git 报 dubious ownership；本轮仅以单次 `git -c safe.directory=…` 读取状态，未修改全局 Git 配置（是否添加 safe.directory 由用户决定）。
- 2026-11-12 用户确认 CLion 改为阶段性集中检查：日常小步修改运行编译器 / 测试 / 格式检查，完整研究迭代、功能接入或里程碑验收前统一检查相关文件；IDE 独有问题、配置问题及用户指定提示按需提前检查，纯文档修改免检。所有 Agent 遵守，协调者可集成后统一检查；异常记录未覆盖范围并继续独立工作，不冒充通过。规则已同步 AGENTS.md 与开发说明。需要诊断时仍先确认前台，逐文件打开再查询。历史 REV-001 诊断证据保留于任务表。
- DEV-014（2026-11-13，Linux，分支 `linux-l2l3` 未推送，包含 `linux-l1`）：用户决定先只推进 Linux。L2 / L3 按 [ADR-0004](docs/decisions/0004-posix-filesystem.md) 实现，compress / verify / extract 在 Linux 上端到端可用，测试广度与 Windows 相同（见 [L2 / L3 记录](docs/linux-l2l3-validation.md)）。用户选定方案 A：非 ASCII 文件名在 Linux 上明确拒绝，Unicode 路径规则另行立项。与 Windows 的剩余差异见 ADR-0004 §9。Windows 侧构建与全量测试已在 3f8d84c 上通过（见 L2 / L3 记录 §7）。2026-11-14 用户同意后合并到 main（`fbeffb4`，合并前先提交了 DEV-013 `1b04a14`）并推送。合并后补做了这些项目，结果见 L2 / L3 记录 §8：
- 两个平台的全量矩阵；
- 两个平台的 fuzz 与 20 次重复测试；
- xfs / btrfs 实测（WSL 回环镜像，由用户挂载）；
- 旧内核实测（印度服务器，6.8.0）；
- CLion 诊断。

仍未做：
- 服务器上的长时间压缩测试（性能 / 压力）；
- POSIX 源文件的 IDE 检查与 clang-tidy；
- 2026-11-14 收尾：WSL 克隆已切回 `main`，同步到 `01cce38`（`linux-l2l3` 已合并，分支保留）。clang-tidy 21 在 POSIX 和 Unicode 源文件上 0 条诊断，探针对照有效（见 [L2 / L3 记录 §9](docs/linux-l2l3-validation.md)）。那处 Linux 残留做过限时排查，无法确定原因（见 [Unicode 记录 §3](docs/unicode-path-validation.md)）。

- DEV-017（2026-11-15，**计划，未实现**）：按用户的定位，Seed 的价值在于“压缩率接近固实压缩的同时，追加文件时免于重新搜索”，优势区间是大文件、无法穷举的场景。计划见 [seed-append-r1](research/ideas/seed-append-r1.md)：对照全量重建、固实、直接搜索 + 分配表、Seed 规则追加和 LZMA2-6；假设 H-ratio / H-append / H-drift / H-generalize。必须先解决的一点：G5 的“按字节排名”分组在插入新文件后不稳定。数据：用户选定本机 + 公开混合；Silesia 语料已获同意并下载（68,182,744 字节，SHA256 `0626e25f…`，zip CRC 全部通过，12 个文件、211.9 MB，只在本机），本机部分用 MSVC / MSYS2 原生二进制。用户随后同意下载 enwik8（zip CRC 通过，解出 100,000,000 字节）和 Linux 6.12 源码（SHA256 与 kernel.org 的 `sha256sums.asc` 一致）。Linux 源码含 Windows 保留名和只差大小写的文件对，改在 WSL 中解包：WSL 一度启动失败（`HCS/0x800705aa`），用户处理后恢复；已解到 `~/corpora/linux/`（86,618 个文件、1.48 GB、62 个树内符号链接）。用户补充了可复现性的两个方面（再次压缩 / 追加，以及解压）；解码端的边界（Seed 必须存进归档，不能由外部提供）和新增假设 H-meta / H-decode-repro 已写进计划 §1.1–1.2。语料 [append-r1](research/datasets/append-r1.md) 已建（2026-11-15）：四个池（linux-src、native-bin、silesia、wiki-text），dev 241.8 MB、holdout 305.0 MB，内容交集 0；每个划分都切成初始集加 3 批追加；3 条路径被 ADR-0005 拒绝（Linux 的 `aux.*`），26 个可能大小写冲突的文件被排除；每个划分都通过了生产清单检查（`seedarc_path_check --manifest` 新增模式，会补上隐含的父目录）。v3 容器和 `append_study` 工具的设计见计划 §7.2（`g5_study` 随 g5-r1 定型，不再修改）。用户决定 Seed 以明文放在文件尾部（§7.3）；“去掉 Seed 当作密码”的想法已说明风险，记为待定、需要 ADR（§7.4）。`seedarc_append_study` 已实现（§7.3.1）：v3 容器，seed / explicit / solid 三种模式，追加时旧包是新包的前缀，固实对照另有整组重压。新增测试 `research.append_study` 和固实编解码测试，在 MSVC、GCC UCRT64、Linux GCC、Linux ASan+UBSan 上都通过（Linux 42/42），0 警告。以上已在 `ae1b994` 提交并推送。用户随后指出尾部单点失效（尾部坏了或还没下载到，整个包就打不开），要求只下载了前面一部分也能先解压前面：决定新做容器 v4，加每代的代头（Seed、组键表、字典）和每个文件的本地头，尾部保留索引副本，不一致就报错；恢复记录以后单独立项（C 级 ADR）。用户又指出代头使各代成为队列：每代可以有自己的 Seed，也可以从旧 Seed 热启动、在旧 + 新文件上重搜。三种追加做法（沿用 / 新代新 Seed / 整体重搜）和新增假设 H-warm / H-queue / H-partial 见计划 §7.5（2026-11-15，未实现）。用户又指出小文件时本地头浪费，要按大小推荐并允许用户自选：代头始终写，第二层分“关 / 检查点 / 逐文件”三档，按估算开销推荐，用户可覆盖，门槛在 dev 上定（§7.5.2）。v4 已实现（§7.5.3，2026-11-15）：新工具 `seedarc_append4_study`，含代头 / 逐文件 / 检查点 / 固实帧四种记录、按头部检查点大小核对位置、做法 A / B 追加、截断恢复命令 `partial`、临时推荐规则 rec-r0（数值未经测量）；共用代码移到 `append_common.hpp`，v3 输出经核对逐字节不变；研究适配器新增 `decompress_solid_prefix`。新测试 `research.append4_study` 和固实前缀解码测试在 MSVC、GCC UCRT64、Linux GCC、Linux Clang ASan+UBSan 上通过（Linux 43/43，Windows 42/42），0 警告；selftest 1242/1242 个损坏样本被拒。已在 `c41d078` 提交并推送。用户又提出分卷压缩：设计记入计划 §7.6（卷头、按逻辑流偏移、追加从新卷开始且不改旧卷、缺卷时跳过空洞；跨卷文件、字典只在一代开头、固实帧跨卷这几处依赖已列出）。用户要求立即实现：已在 `seedarc_append4_study` 中实现 `split` / `vappend` / `vverify` / `vpartial`，含卷头、档案标识、卷链 CRC、三种复写（none / gen / dicts）和缺卷恢复（§7.6.1）；四个平台 0 警告，Linux 43/43，无 sanitizer 报告，selftest 1675/1675。已在 `7961c99` 提交并推送。
- 2026-11-16 集中检查（用户把 CLion 放到前台后）：CLion（MSVC Debug 配置，`-std:c++latest`、`/W4`，`errorsOnly=false`）检查了 `append4_study.cpp`、`append_common.hpp`、`append_study.cpp`、`path_check.cpp`、`core/codec/dictionary.{hpp,cpp}`、`study_common.hpp`、`tests/codec_dictionary_tests.cpp`、`g5_study.cpp`，**0 个错误**。
  - 已修的警告：`append4_study` 7 处可改用 `views::keys/values`、2 处可化简的 `!(a == b)`、1 处可 `constexpr`，以及 2 处“赋值用作条件”（借此收紧 `append` 的参数解析：未知参数、缺值和重复的 `--plan` 都返回 2，已手工验证）；`append_study` 1 处 `views::keys`，1 处按值传参改为 const 引用（v3 输出复核仍逐字节相同，7/7）；`path_check` 2 处移入 `if` 初始化。
  - 保留不改的：`<cstring>` “可能未使用”是误报（直接用了 `memcmp` / `memcpy`）；`DictionaryEncoder::encode` / `DictionaryDecoder::decode` “可设为 const”不改，因为它们通过 `impl_` 改动 zstd 状态；`g5_study.cpp` 随 g5-r1 冻结，其警告（`<cstring>` 误报、未使用的 `budget()`）只记录不改。INFO 级风格提示未处理；其中“冗余 static_cast”在 32 位目标上并不冗余。
  - 改后：MSVC Release 42/42、GCC UCRT64、Linux GCC 和 Clang ASan+UBSan 43/43，均 0 警告，无 sanitizer 报告。
- **MSVC ASan**（`build/msvc-asan-uv` 打开研究工具后重建；确认链接 `clang_rt.asan_dynamic`）：`research.append_study`、`research.append4_study`、`codec.dictionary_research` 通过；在 70 个文件的树上 selftest 1675/1675，无 ASan 报告。stderr 里有 zstd 字典训练器对极小分组的提示（训练失败后回退为无字典），不是错误。
- **热启动搜索**（2026-11-16，用户同意后实现，计划 §7.5.4）：`append4_study search seed|coord START … [restart|local]`，G5 r1 的两种算法按本工具的插入稳定分组重新实现，预算口径同 G5；目标与 `pack` 写出的“字典 + 数据 + 组元数据”逐字节一致（测试核对）。MSVC、GCC UCRT64、Linux GCC、Linux Clang ASan+UBSan、MSVC ASan 均 0 警告且通过。这段新代码的 CLion 复查两次超时，未完成，下次 IDE 在前台时补查 `append4_study.cpp`。
- 以上改动未提交。**尚未**：运行脚本、dev 运行（含 rec-r0 门槛、检查点大小、复写字典的开销）、门槛冻结。（MSVC ASan 和 CLion 检查已于 2026-11-16 完成，见下。）

- DEV-016（2026-11-14 开始）：G5 带预算搜索。用户选定留出集用本机 + 公开混合语料，同意下载 Django 6.1.3，已下载并核对。字典进生产的 ADR 按稳妥路线，等 G5 和固实对照都有结论后再写。计划见 [seed-g5-r1](research/ideas/seed-g5-r1.md)。

- Linux Unicode 路径支持：已作为 DEV-015 完成（[ADR-0005](docs/decisions/0005-portable-unicode-path-profile.md)），待用户验收，状态见下一条。

- DEV-015（2026-11-14，**完成，待验收，未提交**）：用户选定两个平台共用固定版本的 Unicode 规则（简单大小写折叠，Unicode 17.0.0），数据下载已获同意。中途按用户要求暂停过，当天恢复后在最终代码上补齐了全部构建、测试、fuzz 和 20 次重复，结果见 [Unicode 路径验证记录](docs/unicode-path-validation.md) §2.1。改动在 Windows 主检出的工作区里（基于 `3ddc66e`）。临时引用：Windows 仓库的 `refs/wip/unicode`（快照 `457a2d4`，不在任何分支上）、WSL 的 `refs/remotes/win/wip-unicode`，以及 WSL 工作树 `~/SeedArc-unicode`，提交后删除。CLion 诊断已于同日补做。提交和推送须由用户决定。

## 更新约定

开始、完成、阻塞、交接时更新日期、任务行和必要证据；负责人协调共享文件写入，不覆盖其他任务记录。详细日志引用文件，不复制到此处。修改长期目标时同步开发计划；研究结论必须链接对应 run ID。

## 用户想法已读快照

读取日期：2026-11-11；读取者：主 Agent（2026-11-12 种子路线调整前复核：文件清单与两份 SHA256 均未变化）。目录：[docs/users_idea/](docs/users_idea/)。这是一份已读基线，不代表后台监控。后续开发 / 规划、关键决定和交接时重新比较文件清单及 SHA256，变化后阅读并覆盖更新本节，不修改用户原稿。

| 文件 | 已读 SHA256 | 评估与处理 |
| --- | --- | --- |
| [历史 v0.2 构想](docs/users_idea/SeedPack_项目设计方案_v2.md) | `550DD2BA06CE33032850AD6D973E05C0A8F9FC58AEAEE8D1201F71745E61D715` | 基础方向已纳入路线；旧名 / 扩展名 / 阶段示例不作为现行契约，GUI 按用户最新决定在人工交互测试需要时启动 |
| [结构化种子草案](docs/users_idea/SeedArc_结构化种子设计草案.md) | `368AD5B9138F5AB3691DEF36CD0A190D70DDE5EB8086AA956B7D1AA7D6FB376A` | 纳入候选：M1 评估编码 / 版本边界，M4 字段语义与影响隔离测试，M5 随机搜索对照下的局部变异；缓存须测量并验证完整键 / 失效。128 位、字段位宽与搜索方法仍待 ADR / 实验，不提前冻结 |

本轮仅补充开发输入与检查规则，不实现种子 / 缓存、不运行性能实验。重大冲突按 AGENTS 权限模型处理，不能由草案自动覆盖已确认决策。
