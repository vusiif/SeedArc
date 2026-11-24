# DEV-017-r2：研究 v4 资源容量基线

日期：2026-11-23（America/Los_Angeles）。状态：容量与正确性证据已核验；CLion C++ 自动诊断超时，未完成。执行：主 Agent，无委派。下文最初方法为运行前预登记；实际失败、改动与验证追加记录。

目的：在改资源路径前量出当前 v4 在 128 / 256 / 512 MiB 输入上的内存与磁盘压力。采用确定性随机合成内容，分别为两份大文件与 4 MiB 文件集合；初始 70 % 加新文件 30 %。**这不是压缩收益或 Seed 价值实验**，不读取公开 dev / holdout，不据此推广性能结论。

工具：只读复用先前印度 Release 二进制 `e171669b1b984280005e5f9fafe5c43065252ea48b6bda637e032614f5654c24`，源码基线 479eab9、同一 vcpkg 锁定依赖。每个子进程固定同一 CPU；使用技能 shared 的 3 GiB 内存 / 2 核 / Swap512 MiB，而非无上限运行。相邻规模按上一步峰值线性放大再加 20 % 余量，预测超过 2.5 GiB 则停止上探，不擅自放宽资源限制。

固定计划 0x0（单组、无字典），分别 pack / verify / append / verify-all，比较 seed segment 与 solid regroup 的资源；每个产物独立校验，旧包 SHA256 不变。记录每条工具 JSON 的 peak_memory_bytes（Linux ru_maxrss）和磁盘逻辑字节数；磁盘统计按技能监控口径对硬链接的每个名字计数。每个 case 完整验证后只清理其新建数据和产物，逐条 JSONL、输入 / 产物哈希与原始日志保留。

预计峰值逻辑磁盘约 3.7× 输入（512 MiB 约 1.85 GiB，加小型日志），不上传或新装工具链；如果任一步触发限额 / OOM / 失败，保留现场，不将未运行的规模写成通过。后续根据实际峰值决定最小必要的复制 / 校验改动，再做相同输入的前后对照；归档字节应保持一致。

## 实现与边界

仅修改研究层 `research/tools/append4_study.cpp` 与对应 CMake 测试，不改生产归档格式 / Seed 解释 / 输入上限。显式指定冗余等级时只构造指定布局并移动 Pending；逐块校验使用 64 KiB 缓冲区，固实校验一次解码一个帧；追加只比较新输入，但仍完整解码并 CRC 校验旧载荷；regroup 移动旧文件缓冲区而非再复制。新增空文件、多块文件与损坏旧载荷回归覆盖，未弱化分卷丢失断言。

这仍非完整流式实现：输入目录和归档仍整批加载，单固实帧可能很大，regroup 仍持有全部原文；自动冗余选择仍构造三种布局。512 MiB 合成输入通过不证明 >1 GiB、真实多代分支调度、字典搜索或主性能实验已就绪。

`verify` 新增 input_read_wall_ms / input_read_cpu_ms 与 verification_mode；decode_wall_ms 包括比较、排除输入目录读取，与旧字段计时口径不同。本轮不据该字段比较速度，不作统计显著性或 Seed 收益结论。

## 固定环境与实际任务

- 基线提交 `479eab9a524ff0cf82ef55f36dc19ea4d06f77fc`，改后源码为该提交加 [运行差异](post-source-patch.diff)，运行输入哈希见 [快照](post-source-hashes.json)。同一印度主机、GCC 14.2、CMake 3.28.3、Ninja 1.11、zstd 1.5.7 / liblzma 5.8.4；依赖来自先前任务的 pinned vcpkg 安装树，未系统安装工具。
- 基线任务 `20261123T132143Z-b79a793d`：exit0、日志未截断，128 / 256 MiB 共 4 case / 32 命令全成功；512 MiB 预测 RSS 3,066,121,420 B 超过 2,684,354,560 B 门槛而未运行。fetch SHA256 `82e5f749f39b02fe89b4bf11d4b1b7728f29b30c604e8b8a308359d99a8c92c5`，已 clean，仅保留结果 / 日志 / 状态。
- 改后集成任务 `20261123T134340Z-d28b6277`：Release 与 ASan+UBSan 全量通过后，容量阶段在 512 MiB Seed 追加后触发磁盘监控；exit125 / disk_limit，不能计为整轮成功。已保存 [结果状态](post-result.json)、[未完成原始数据](after-interrupted/raw.jsonl)，共 35 条已完成命令 return0，512 MiB case 尚未 verify-all。其 probe 自身磁盘不足 2 GiB，但同任务还含构建和历史产物；不能只计算 probe 子目录来声称整任务满足磁盘预算。fetch SHA256 `bec8d2175a674bbbe4fc3e2948e3a2d9cce2383b3dee5758967ce6b5c0548e5d`。
- 调整为独立容量任务 `20261123T140842Z-51eacc6a`，只读复用同一改后二进制 `7b0bb1dec8e466a84d820b82ecf43b451c46bb4f8fe4f85a9f509615eb25a96c`，不携带构建 / 历史产物，未改输入、计划或资源限制。2026-11-23 14:09:54–14:14:10 UTC 正常 exit0，日志 45,324 B 未截断；128 / 256 / 512 MiB 两布局均完成。最终原始证据与比较核验见后续补充。

源码与二进制均可追溯到上述归档。完整命令见归档中的 task.sh、logs/combined.log、results/*日志以及 probe/raw.jsonl；每条命令记录 argv、退出码、工具 JSON、RSS 和磁盘逻辑字节。原始归档 / SHA 回执保存在 `research/local/india-append/artifacts/`，任务回执在同级 receipts/。中止的旧任务 `20261123T123105Z-d5596074` 提供固定工具链，状态未知清理保护仍存在，未绕过删除。

## 正确性与安全验证

- Windows MSVC Release 全量 **42/42**（75.33 s），见 [宿主日志](windows-native-ctest.log)。先前沙盒 11 项文件系统测试因 Windows error5 访问拒绝而失败，日志保留；在正常权限下复测同一测试集通过，没有关闭检查。
- 印度 Release **43/43**（79.50 s），ASan+UBSan Debug **43/43**（396.90 s），见 [Release](post-release-ctest.log)、[Sanitizer](post-asan-ctest.log)。两份构建日志未发现 warning / error；无 sanitizer 报告。仅已有 fuzz 语料重放，未新增持续模糊测试。
- Windows 旧 / 新 MSVC 两遍独立历史各 34 产物 / 15 搜索，旧 / 新 34 个归档哈希完全一致，见 [兼容性](windows-compatibility.json)。印度新工具两遍同样通过，含中文路径、独立父历史、坏清单及超限拒绝，见 [功能验收](post-acceptance.json)。资源前后相同输入 / 归档的机器核验使用 `research/scripts/append_resource_compare.py`。
- 本机初次构建的 vcpkg post-build 使用了不可执行的 WindowsApps PowerShell 别名，构建缓存改为真实 pwsh 路径后通过；仅本地生成缓存，未改项目依赖或工具链标准。

## CLion 与用户人工提示

CLion 2026.3.0.1 确认正确 SeedArc 项目、MSVC Debug、`/W4 /permissive- /Zc:__cplusplus /utf-8 -std:c++latest`，CMake 文件诊断 errorsOnly=false 完整返回无问题。C++ 文件两次分别 30 / 60 s 超时，空结果不是通过，已停止重试；证据见 ide-*.json。用户随后提供人工提示：未使用 include、cache / digits 作用域、冠词、连续点、英美拼写、代码词拼写。

逐项核对后，所有直接 include 均在本文件实际使用（含 SplitMix64、memcpy / memcmp、iostream、views、tuple），保留直接依赖；保留 vappend / vverify / vpartial、协议字段缩写与 nullopt，不改协议或禁用拼写检查。cache 改为 range-for 初始化变量，生命周期仍覆盖整个循环而非每次迭代；digits 移入循环；修正英文注释。该整理在印度二进制测试之后进行，差异另见 [提示整理](after-remote-hints.diff)，最终完整差异与源码哈希也单独保存；不冒充最后文本版本已跑 Linux sanitizer。

整理后 MSVC 目标重建成功，定向 research.append4_study **1/1**（16.10 s），见 [定向日志](windows-hints-targeted.log)；clang-format --dry-run --Werror 与 git diff --check 通过。CLion C++ 自动检查依然未完成，用户人工列表不能替代完整自动诊断覆盖。

## 后续与授权

用户 2026-11-23 明确允许后续实验使用印度服务器全部资源，可按 `in-long-task` 独占模式使用全机 CPU / 内存 / Swap，避免和其他技能任务并发。本轮保留原 shared 条件完成前后比较；未宣称已运行独占测试。技能独占默认仍有 2 GiB 磁盘预算，后续多历史主实验须先设计整体磁盘预算 / 串行保存与清理，不能只放开内存而忽略磁盘；不读 holdout，不自动扩大语料授权。

下一步仍为真实公开 dev 数据的主实验准备：先处理多分支历史磁盘调度、当前可分参考搜索的实测预算，再决定大规模和策略空间；不为扩大空间人为制造耦合，不用本轮随机数据证明 Seed 价值。用户两份 users_idea 原稿哈希复核未变，最新 seed-append-r1 后续讨论继续作为方向输入。

## 最终资源核验

独立容量归档 SHA256 `1e0d2bd7c519b68e63d2ce6fd5868c66c917394d5654210a438c5924571a1b30`，技能 fetch 已核验。主 Agent 从归档提取 [after 原始数据](after/raw.jsonl)，检查全部 **48 条命令退出 0、6 个完整 case**，无部分 case；每 case 两种模式的打包 / 初始校验 / 追加 / 全量校验均在完整记录中。与 [before](before/raw.jsonl) 四个共同 case 的输入清单、seed / solid 两种旧新归档大小及 SHA256 完全一致。基线 512 MiB 未运行，不能比较其前后降幅。

复现：`python research/scripts/append_resource_compare.py research/experiments/20261123-append-resource-main-001/before research/experiments/20261123-append-resource-main-001/after`，结果见 [comparison.json](comparison.json)。每 case 只测一遍，数值是进程峰值观测，非置信区间。

| 输入规模 / 布局 | 基线峰值 RSS (B) | 改后峰值 RSS (B) | 降幅 |
| --- | ---: | ---: | ---: |
| 128 MiB / 两大文件 | 645,718,016 | 551,628,800 | 14.57% |
| 128 MiB / 4 MiB 集合 | 646,012,928 | 551,833,600 | 14.58% |
| 256 MiB / 两大文件 | 1,276,936,192 | 1,089,155,072 | 14.71% |
| 256 MiB / 4 MiB 集合 | 1,277,550,592 | 1,089,683,456 | 14.71% |
| 512 MiB / 两大文件 | 未运行 | 2,163,949,568 | 不可比较 |
| 512 MiB / 4 MiB 集合 | 未运行 | 2,164,707,328 | 不可比较 |

512 MiB 逻辑磁盘峰值分别 1,986,757,353 / 1,986,764,063 B（约 1.85 GiB，probe 子目录统计）。把容量负载和构建 / 历史产物分开后整个技能任务未触发限额；不保证七分支主实验同时保留也可满足。格式、diff 与 Python AST 检查通过；本轮改动未提交或推送。

收尾：基线、集成失败与独立容量三个任务均在 fetch / SHA 核验、独立检查后 clean，远端只清理各自 work/tmp，结果 / 日志 / 状态保留；当前无本轮运行任务。旧中止工具链任务仍遵循其 unknown 清理保护保留。最终工作区源码与印度测试源码之间只有上述提示整理差异，可在 final-source-patch.diff / final-source-hashes.json 核对。
