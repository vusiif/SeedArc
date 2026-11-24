# DEV-017：印度固定主机公开 dev 首轮

日期：2026-11-23（America/Los_Angeles）。状态：因研究定位复核而主动中止；Release 基线通过，完整性能实验未完成。主 Agent 执行，未委派。

## 目标与方法（运行前登记）

先建立印度 `vps-in1` 的 Release 基线，验证当前代码，再探索追加与冷 / 热启动的大小和计算成本。只使用 append-r1 的 linux-src / wiki-text **dev**，按已提交的 TSV 重建并逐文件核对；不读取 holdout 清单、不上传本机专有二进制。这里只探索，不冻结 H-ratio / H-append 等假设的数值门槛，不据此做留出结论。

### 定位复核（启动后，2026-11-23，用户再次强调）

用户早已将重点转向“大体积、无法充分遍历的搜索空间、快速追加与策略复用”，见 seed-append-r1 §1 / §2 / §7.5.1；中小体积和小候选空间上的 G5 负面结果不能外推否定这一方向。本轮每池仅约 50 MB，仍使用最多 8 组的小型计划空间，**仅作为印度固定环境与追加运行流程预演**，不能充当用户核心设想的代表性验证，不能据此决定 Seed 长期价值。数据字节数大与候选空间复杂是两个独立变量，后续主实验须分别设计并报告，不通过重复小样本取代。

主实验前还需要明确大规模语料的规模梯度、候选之间的耦合 / 缓存失效、有限预算下实际探索覆盖、连续 A / B / C 历史和完整重写成本；研究工具当前整批入内存且输入上限 1 GiB，不能直接把限额抬高当成大规模支持。保留本轮真实结果，不自动取消、重启或把它扩大为留出评估。

- 源码：`479eab9a524ff0cf82ef55f36dc19ea4d06f77fc` 的必要文件，以 Git 对象打包，不携带 `.git`。打包时工作区仅有本轮计划 / 状态修改及旧调试日志；源文件来自指定提交。
- 执行脚本：`research/scripts/append_india_{bootstrap,dev}.py` 与 `append_india_task.sh`；配置 [append-india-dev-r0](../../configs/append-india-dev-r0.json)。实际上传脚本与输入哈希在远端任务 work 和本机暂存目录中；取回时保存进结果。
- 环境：Ubuntu 24.04 主机原先没有 PATH 内可用的 C++ 编译器 / CMake。只下载可信系统索引解析出的 `.deb`，验证 SHA256，解包到任务目录，**不执行系统安装**；vcpkg 使用仓库钉住的 baseline，按 manifest 编译 zstd / liblzma。不改 C++23 或警告 / 测试标准。工具链包版本、源码 / 二进制哈希和 CMake cache 写入 results。
- 基线：Release 全量构建和 CTest，失败则停止，不运行性能实验。
- 比较：初始 8× 输入字节搜索预算，追加阶段 4×；三个重复 / PRNG 种子 3、11、29。初始共用计划；三批追加逐次对照沿用 Seed、显式表、固实新段、固实重压、固定计划全量重建、生产 LZMA2-6，以及 seed / coord 的冷 / 热启动与当前批次新代搜索。
- 边界：新代搜索本轮从“沿用 Seed 链”的旧包追加当前批次，并非独立持续 B 链。coord 的暖起点与 seed 相同（上一轮 seed 暖搜索得到的计划），不等价于各自独立历史。完整生产 LZMA2-6 与研究容器格式 / 安全提交成本不同，单列，不混称同格式比较。分卷、截断和推荐规则调参不在首轮范围。
- 计量：一个 benchmark 子进程、固定在一个允许的 CPU；热缓存、方案顺序按重复轮换。主指标为完整产物字节、独立进程墙钟 / CPU、搜索与编码分项（工具 JSON）、峰值 RSS（工具 JSON）。每个产物再用独立 verify 比较全部文件；LZMA2 另解压逐文件哈希核对。验证时间单列，搜索 + 打包时间合并时明确标注。
- 数据保存：逐命令 JSONL / stdout / stderr、输入校验、环境、产物大小与 SHA256、阶段性进度实时落盘；大产物核验后删除，保留可重建输入和脚本。三个随机种子的输出可以不同，报告分布，不误称重复同一计划。

## 远端执行

- task ID：`20261123T121924Z-53f9033d`。
- 模式：exclusive，最长 6 小时；保留技能 2 GiB 磁盘与 64 MiB 合并日志限制。构建 parallel=2，计时单线程 / 单 CPU。
- 远端：`/home/sc/long-tasks/20261123T121924Z-53f9033d/`。
- 本机回执：`research/local/india-append/receipts/20261123T121924Z-53f9033d.json`。
- prepare：成功，必要输入合计 1,722,614 B（约 1.7 MB），不含原始语料。源码归档、运行器与清单在 `.sandbox-test/in-append/inputs/`。
- start：已执行一次，systemd 返回 `ActiveState=active` / `SubState=running`、`User=sc` / `NoNewPrivileges=yes`，exclusive 资源模式生效。
- 下一步：持有该 ID 检查；完成后 fetch、核验本地归档，再清理该任务 work/tmp。失败先取回日志并保留现场。

## 结果

当前仅有阶段性证据，不作完整性能结论。无生产 C++ 修改，已有研究工具的 IDE 未完成项保留。

### 已核对的当前阶段

尝试 3 的 Release 构建成功，build.log 中 `warning:` 计数为 0；CTest **43/43**，耗时 67.80 秒。公开 dev 输入 linux-src 3,625 文件 / 50,331,389 B，wiki-text 50 文件 / 50,000,000 B，逐文件大小与 SHA256 均已匹配原清单。二进制哈希、工具链包版本与 CMake cache 已在远端 results 保存。

只读快照见 [baseline-observation.json](baseline-observation.json)：rep 0 的 linux-src 第一批追加的各对照均已执行并核验（包括 LZMA2 提取比对、冷 / 热搜索打包验证），目前进入第二批。systemd 任务尚未给出最终 result；不把阶段性成功当成全轮通过，尚未汇总性能数字。完成后仍须核对退出状态、所有 JSONL、三轮结果、取回归档哈希以及是否截断。

清理：尝试 1 已在取回核验后成功 clean，仅保留结果 / 日志 / 回执；尝试 2 的 clean 请求 SSH 断开，后续核对 work 仍存在，清理未完成。为了不在当前计时期间制造额外磁盘活动，延后到本轮结束再清理；不得把它误记为完成，也不得重启旧任务。当前运行的尝试 3 不清理、不重复 start。

### 准备尝试 1（失败，保留证据）

`20261123T121924Z-53f9033d` 于 12:20:51–12:22:08 UTC 运行，exit_code=1、日志未截断。37 个工具链包已在任务目录下载、核验和解包，但 vcpkg bootstrap 要求 `VCPKG_DOWNLOADS` 预先存在，脚本没有创建目录而失败；没有构建或性能结果。修正为先建立 cache / vcpkg-downloads。

实际输入与失败日志已取回 `research/local/india-append/artifacts/20261123T121924Z-53f9033d.tar.gz`，SHA256 `1d3c57cd547079f899484eed5179a184e150d17b249024d3b5d34d37e5cba0b6`，技能校验通过。保留远端现场，未清理。

### 准备尝试 2

新 ID `20261123T122455Z-3c407ddc`，相同源码与实验配置、修正后的 bootstrap；旧任务已明确结束后才 prepare。回执在同一 receipts 目录。后续只检查新 ID，不重复 start。

本次 exit_code=1（12:25:52–12:27:57 UTC，日志未截断）。工具链与 manifest 依赖均成功，liblzma 5.8.4 / zstd 1.5.7 已构建；项目 CMake 配置失败，因为最小源码快照漏打包 `research/tools/`，没有编译 / 测试 / 性能结果。失败归档 SHA256 `351dbef16576e79afb9349d50a216dc842485276f60355e2dd90bc2d7e06202f`，已通过技能 fetch 核验，位于同一 artifacts 目录。

### 准备尝试 3（已中止）

修正打包范围，加入 `research/tools`，并在本地核对根 CMake 的每个 add_subdirectory 及六个子构建入口均在 zip 内。必要输入共 1,809,455 B，暂存于 `.sandbox-test/in-append/inputs-v2/`。新 ID `20261123T123105Z-d5596074` 已 start 一次，systemd 返回 running / User=sc / NoNewPrivileges=yes。实验配置与生产源码提交不变；持有此 ID，尚未有实验结果。

## 中止与归档（最终状态，2026-11-23）

用户询问本轮是否仍需要继续；复核后判断固定环境与流程已得到验证，剩余相同规模的重复不能回答最新的大规模 / 难穷举主张，因此停止剩余任务。systemd 日志确认 **12:48:28 UTC（04:48:28 America/Los_Angeles）** stopped / deactivated；随后 /proc 核对无带该任务 ID 的残留进程，单元 inactive / dead / not-found。

技能 stop 杀掉了父 runner，没有生成正常 result.json；fetch 因 `Completion unknown; diagnose before cleanup` 拒绝打包。没有伪造 result.json、没有重启。诊断停止后，只读取得 results / logs 的 586 个普通文件，逐个核对远端 SHA256（没有移除工具的状态保护或删除现场）。本地完整目录：`research/local/india-append/interrupted/20261123T123105Z-d5596074/`，含 `verified-files.json`。关键小型证据在 [evidence/](evidence/)，停止日志见 [stop-diagnosis.json](evidence/stop-diagnosis.json)。

停止前写完的命令记录 **241 条、0 个非零退出**；观察到 rep 0 / rep 1，最后完整记录为 rep 1、wiki-text、第二批、rebuild-solid 的 verify。正在执行时被中断的子命令可能没有完整行，不计为通过。三轮配置没有完成，没有 pilot-complete 标记，不给完整性能结论，不以这轮否定或支持 Seed 的大规模价值。

尝试 2 已在停止计时后成功 clean，尝试 1 / 2 均保留远端结果、日志与本地已验证归档。本次中止尝试的 work/tmp 仍保留，因为技能的取消后 unknown 状态没有安全的正常清理入口；此项未完成，不直接递归删除绕过保护。下次接续先处理停止状态与清理，再设计符合最新方向的大规模 / 复杂空间主实验。当前没有本轮运行中的任务。
