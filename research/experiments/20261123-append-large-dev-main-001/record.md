# DEV-017 r2：第一轮大目录追加 dev 成本测量

日期：2026-11-23（America/Los_Angeles）。执行者：主 Agent，无委派。状态：首任务中断已诊断；续测完成并取回核验，9 分支 / 34 产物的数据聚合完成。属于跨两任务的单遍 dev 成本观测。

## 运行前方法与边界

问题：在已通过 512 MiB 容量验收的研究 v4 上，真实源码追加时，A 沿用、B-new 新代搜索、C 合并热搜、直接坐标对照与固实两种追加路径的完整大小和总成本有何差异？当前可分空间在大输入下的预算、独立组评估和实际时间是多少？不预设 Seed 有收益，也不以形式约 2690 万计划证明难搜索。

输入：[append-linux-large-dev-r2](../../datasets/append-linux-large-dev-r2.md) 的 512 MiB 清单，536,864,155 B / 30,134 文件，初始 + 3 批同来源追加。固定源归档 SHA256、逐文件 SHA256；不读取留出内容，不上传本机专有文件。单个大文件与内容漂移另测，不宣称本轮覆盖。

初始计划 `0x148`（沿用 r2 功能验收的固定起点：dir1、3 组，字典依次 64 KiB / 16 KiB / 无字典，非零字典的训练采样上限均为每文件 64 KiB），所有分支相同起点，初始 pack 的成本单独报告；本轮不计为初始搜索优化。B-new 从自己上一计划在新批次 local；C / C-coord 从自己上一计划在累计输入 local；R-seed / R-coord 从 default（实际编码 `0x40`：一组 64 KiB 字典）restart 重建，是同算子冷启动参照。A / D-nearest / S-segment 延续各自历史；S-regroup 仍重压全部组。B-new 不代表可引用旧字典的 B-mix。

首次只用预算 4× 当前输入字节、PRNG seed=3、1 遍，属于探索成本测量；不是冻结主评估、统计显著性或质量—预算曲线。根据本轮实际成本，再安排预算梯度、重复与顺序轮换。固定串行子进程和 CPU，温热当前输入后调用，独立记录过程墙钟、工具内部墙钟 / CPU / RSS、实际预算与候选轨迹、完整产物各部分字节。

采用用户已授权的印度独占模式，构建可用主机 CPU，基准每次一个子进程固定 CPU，避免并发噪声。技能仍保留 2 GiB 磁盘预算和 1 小时单轮时限（任务留出构建 / 获取时间）。新调度器每次只保留一个分支的旧新包，输入目录通过有界硬链接组成，按技能口径把每个链接名字计数；提前检查输出的保守原文加元数据空间预留。校验并保存 JSON / 哈希后只回收本任务产物。任何超限、工具异常或校验失败均停止并保留证据，不静默减检查或扩大输入上限。

源码基线 479eab9 加已有资源补丁和用户提示整理，沿用固定 GCC14.2 / pinned zstd1.5.7、liblzma5.8.4；新运行器与输入快照哈希随任务保存。先运行 Linux 定向研究 CTest 与新旧调度器小夹具交叉比较，再测真实 dev；不重复未改生产代码的全部测试，不冒称完整阶段 / CLion 验收。

## 完成条件

小夹具 Windows / Linux 的新旧调度器各 34 产物哈希一致、两遍确定性、中文路径、独立父历史、冷启动无暖父计划、坏清单拒绝、临时分支回收；真实数据逐文件哈希与生产 manifest 校验通过，每个产物逐字节校验，A / B-new / D / S-segment 旧包前缀及代数正确。远端真实退出成功、日志完整、fetch / SHA 校验和本地独立分析后才报告该轮完成。若失败，也保留原始记录和诊断，不补填缺失结果。

## 当前进度

三份 128 / 256 / 512 MiB 清单已在 Windows 生成并通过生产路径检查，users_idea 两份原稿哈希未变化。新调度器未修改 8 MiB 功能运行器上限；预先复核原大规模 r2 方案与最新 seed-append-r1 §7.5.1。远端 preflight 有 71.9 GB 空闲、sudo 可用，无运行技能任务；旧 unknown 任务仍保留，不删除或重复启动。

## 实际启动

Windows 新调度器两遍均通过，每遍 34 产物和 83 条工具命令；与独立历史参考产物哈希一致，证据 [windows-scheduler-acceptance.json](windows-scheduler-acceptance.json) / [histories](windows-scheduler-histories.jsonl)。Python AST 四文件通过。

印度任务 `20261124T051435Z-cef36683`（任务 ID 使用 UTC，当地日期仍 2026-11-23），exclusive，任务时限 2 小时，其中大数据运行器单轮 subprocess 上限 1 小时。输入包 5,730,608 B，小于 16 MiB 上传限制；[输入哈希](input-hashes.json) 保存源码 zip、工作区 C++ 补丁、新脚本与清单。回执：research/local/india-append/receipts/20261124T051435Z-cef36683.json。工具链只读复用原中止任务的固定目录，其他已清理任务无依赖。

启动后复核编码说明：预登记文字曾误把 `0x148` 称为一组 G5 默认，实际 seed_revision=1 解码是三组混合字典；已在结果出现前更正说明，未改运行命令。冷启动 default 才是 `0x40`。本轮暖起点是固定计划随各自历史演进，并非在初始集充分搜索得到的最优计划，不用于宣称成熟初始 Seed 的 H-warm 已验证。

## 已完成的执行前验收

Linux GCC14.2 重建 0 warning，定向 CTest research.append4_study **1/1**（10.08 s）；新旧调度器两遍各 34 产物一致，已通过正常 SSH 只读取回六份检查文件并逐个 SHA256 核对。Windows / Linux 的 34 条历史记录所有字段完全一致（含归档哈希、起点、父哈希和代数），见 [跨平台核验](cross-platform-scheduler.json)、[Linux 验收](linux-scheduler-acceptance.json) / [定向 CTest](linux-targeted-ctest.log)。这是主 Agent 的独立文件比较，不只引用执行器自述。

全部 30,134 个真实输入已在远端核对归档与逐文件 SHA256，生产 manifest 再检查为 ok / 33,906（含父目录），见 [输入核验](linux-inputs-verified.json)。metadata 可重复生成、三规模路径严格嵌套且各规模内容哈希唯一，见 [清单复核](manifest-check.json)。

## 运行中发现的解释限制

通过清单元数据核对当前规则实际键数：dir1=1、dir2=25、ext=100、size=3，见 [key-diversity.json](key-diversity.json)。所有路径带 linux/ 根前缀，使 dir1 退化为一个键；329 个源码来源组不能当作 329 个 Seed 候选组。初始 0x148 虽声明 k=3，也不代表三个非空内容组。本轮保留已预登记路径和命令，不根据中间结果换规则 / 删除前缀；这个事实限制对分组探索和 H-warm 的推广，后续可单独评估真实相对根路径与规则修订，但不能静默混入本轮或人为扩大空间。

## 中断、诊断与续测

原任务 `20261124T051435Z-cef36683` 在 D-nearest 完成后，技能 runner.py 的磁盘统计 `is_file()` 与 `lstat()` 之间发生文件删除竞态，FileNotFoundError 未捕获导致 unit exit1，systemd 随之结束实验子进程；不是压缩器报错 / OOM / 磁盘超限。实际 journal 见 [monitor-failure-journal.log](monitor-failure-journal.log)。无正常 result.json，技能仍判 unknown；已确认无相关 PID、unit failed，不能伪造完成记录或使用正常 clean。

只读取回 122 个结果 / 日志文件，逐个 SHA256 核验，详细清单和原件位于 research/local/india-append/interrupted/20261124T051435Z-cef36683/。诊断见 [interrupted-diagnosis.json](interrupted-diagnosis.json)，原始 49 条已完成命令 return0、20 个逐字节校验产物归档于 [interrupted/](interrupted/)。A / B-new / C / C-coord / D-nearest 五分支完整，S-segment 未产生完成结果。未知状态的原任务 work/tmp 保留，不绕过清理保护。

修复只应用于新任务专用 runner.py 副本，不修改用户已安装的 skill 或系统服务，不改磁盘 / CPU / 时间限额。改为单次 lstat 数普通文件，按硬链接名字逐个计数，仅忽略遍历期间已经消失的文件 / 目录，其他错误继续传播。补丁 [monitor-fix.diff](monitor-fix.diff)；本机两次检查覆盖硬链接计数、删除竞态和权限异常不被吞掉，见 [monitor-check.json](monitor-check.json)。原 / 新监控哈希与不变的 2 GiB 限额见 [continuation-monitor-patch.json](continuation-monitor-patch.json)。

续测任务 `20261124T054749Z-aef206e4` 为 exclusive，仅补 S-segment / S-regroup / R-seed / R-coord；其实际上传的 append_large_dev.py、二进制、512 MiB 清单均固定核对为原哈希。新任务将原数据逐文件复制进自己的 work/data 再核验，磁盘工作集仍计入本任务，不通过共享数据位置规避预算。原任务已完成的五分支不重复测量。两任务的观测明确分开归档；时间比较属于中断后的单遍探索，监控实现变化也可能影响主机噪声，不作显著性结论，后续预算曲线需统一修复后的监控再重复。

## 最终核验与结果

续测 `20261124T054749Z-aef206e4` 于 2026-11-24 05:50:08–06:05:02 UTC 正常结束（当地 2026-11-23），exit0，34 条命令 / 14 个逐字节校验产物；日志 4,395 B 未截断，无遗留子进程。fetch 归档 SHA256 `2c5373617c698bdd1c7e75ca22cdc19e3350a0ab05061c3b03ed2154ea4734bb` 已核验，原始结果见 [continuation/](continuation/)，状态见 [continued-result.json](continued-result.json)。归档与回执在 research/local/india-append/artifacts/。

主 Agent 从两份原始证据按分支聚合并独立检查：**83 条成功命令、34 个产物、9 条独立历史、15 次搜索**；检查每批父哈希 / 计划、代数、前缀、预算未超、搜索结果不劣于起点、独立校验成功以及物理归档字节精确对账。两任务的二进制 / 运行器 / 清单哈希、CPU、预算、种子均一致；聚合保留每条原命令的 task/index，见 [aggregation.json](aggregate/aggregation.json)。这是派生分析，不为原任务伪造 result.json 或完成状态。

复现：

```text
python -B research/scripts/append_large_merge.py research/experiments/20261123-append-large-dev-main-001/interrupted research/experiments/20261123-append-large-dev-main-001/continuation --output <new-aggregate>
python -B research/scripts/append_large_report.py <new-aggregate> --output <new-analysis.json> --markdown <new-preview.md>
```

机器结果见 [analysis.json](analysis.json)，完整九路径表见 [preview.md](preview.md)。三批新增阶段的工具进程墙钟合计（含内部验证，不含初始打包 / 准备 / 预热 / 独立 verify）：

| 路径 | 最终包 (B) | 搜索 + 写包 / 追加 (s) |
| --- | ---: | ---: |
| A 沿用 | 85,821,177 | 23.75 |
| B-new 新代搜索 | 86,047,579 | 61.56 |
| C 整体热搜重建 | 80,521,172 | 250.41 |
| C-coord 显式坐标热搜 | 80,551,346 | 261.14 |
| D-nearest 显式表追加 | 85,922,057 | 31.08 |
| S-segment 固实分段 | 65,818,554 | 27.42 |
| S-regroup 固实重压 | 60,705,545 | 76.76 |
| R-seed 冷搜重建 | 80,521,172 | 247.61 |
| R-coord 显式坐标冷搜 | 80,551,298 | 224.01 |

输入为 Linux 6.12 同一静态源码版本按哈希人工分批，非真实版本演进。仅一个种子 / 预算 / 重复；跨两任务更换过监控实现，以上是成本观测，不作统计显著性或未测平台的时间推广。源码小夹具 Windows / Linux 可复现，不等于真实大目录已做跨平台性能实验。

### 解释与可证伪结论

1. 本轮**固实分段是更强的简单基线**：比固实整包重压大 8.42%，追加工具调用成本约为其 35.72%；两者在同一续测任务中测量。相对 A，包小约 23.31%，调用成本仍为同一数量级。它已提供接近固实大小与较快追加的可用对照，不能把这种能力直接归为 Seed 独有价值。
2. B-new 比 A 多 226,402 B，新增字典 196,608 B、数据多 30,010 B，部分记录少了 216 B；新增字典占净增加量约 86.84%。本轮没有显示新代搜索值得额外成本；**B-mix 未实现**，不能将 B-new 的结果套用到“每组选旧字典或新字典”的用户方案。
3. C 比 A 净少 5,300,005 B，但主要来自清除 **5,572,240 B 旧索引尾部**；压缩数据反而多 272,811 B，代记录 / 当前索引少 576 B。这不是 Seed 找到了更好的数据编码。应加入固定计划直接重建 / 结构整理对照后再讨论“整体优化收益”。
4. C 与 R-seed 的最终归档 SHA256 完全一致，三次搜索的实际预算消耗也分别完全相同。本轮未观察到 Seed 热启动节省编码预算。Seed 搜索在 B 首批和 C 首批各仅省 8 B 评估目标，其余 Seed 搜索从起点到终点省 0 B；不意味着其他预算 / 起点也无效。
5. 每次搜索只实际评估 2 或 4 个组配置、2 或 4 个计划，耗时对大输入确实达到数十秒，但不构成全局覆盖或“难穷举空间”的证据。dir1 单键、最多 8 组与目标可分等限制仍成立，不能把这次结果推广为大空间 Seed 的最终判断。

最大观测任务逻辑磁盘 1,303,214,150 B；两阶段各自未触发磁盘限额。最高工具 RSS 为固实重压的 1,754,210,304 B。独立 verify 三批合计每分支约 22 s，已在完整表单列。整体仍加载输入 / 归档及固实帧，不宣传完整流式能力或 >1 GiB 验收。

### 收尾和下一步

续测任务在 fetch、SHA 和独立分析后 clean，保留远端结果 / 日志 / 状态 / 任务源码；原中断任务没有正常完成标记，work/tmp 仍保留，不绕过 unknown 清理保护。全局 in-long-task skill 没有修改，后续任务需沿用已验证的任务专用监控补丁或另行维护该 skill。补丁执行脚本与任务入口已保存在本记录目录，分析工具哈希见 [analysis-tools-hashes.json](analysis-tools-hashes.json)。

下一步先增强对照与输入规则：将固实分段作为必备基线，分开重建清理旧索引和改变压缩计划的收益；用新的清单版本评估去掉人为公共根前缀后的分组效果，保留本轮原数据与原路径。确认有实际压缩用途后，再决定帧布局 / 分组 / 顺序等候选是否需要新语义修订；不只扩大 D/T 搜索预算或人为堆砌空间。随后才做统一修复监控下的预算梯度、重复、漂移，以及冻结后的独立留出。用户两份 users_idea 原稿哈希再次核对未变，最新 seed-append-r1 后续讨论仍是方向依据。

本轮未修改生产格式、C++ 或依赖，没有新增完整构建矩阵 / sanitizer / fuzz；复用已有证据并实跑 Linux 定向 CTest 与跨平台调度器验收。未重复 CLion C++ 检查（本轮无新 C++ 改动）；不覆盖前轮自动诊断超时限制。工作区尚未提交或推送。

更正（DEV-019，2026-11-23）：按append_common.hpp真实group_key复核，旧清单dir2键数为22；原辅助key-diversity.json的25将root层文件误作额外二级键。原始辅助文件保留以便追溯，压缩使用C++规则，其字节 / 耗时及dir1=1结论不受影响。复核脚本solid_search_manifest.py及solid-linux-dev-r1/metadata.json保存更正证据。
