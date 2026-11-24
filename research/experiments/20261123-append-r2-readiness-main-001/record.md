# DEV-017-r2：主实验准备与独立历史验收

日期：2026-11-23（America/Los_Angeles）。性质：设计审查、现有目标成本分析、运行器功能验收；**不是大规模性能实验**。执行：主 Agent，无委派。

## 目标与变更

依据测试后的最新用户方向，新增 [主实验 r2 方案](../../ideas/seed-append-main-r2.md)，逐项核对现有 append v4 实现。源码基线为 `479eab9`；本轮未提交修改涉及 STATUS、追加文档及 Python 运行器 / 分析脚本，没有修改 C++、依赖锁或生产 / 研究容器格式。精确源码哈希见 [source-hashes.json](source-hashes.json)。运行器快照见 `source/`。

已纠正的调用语义：旧 pilot 的 B 从 default 冷搜并接在 A 历史上，C-coord 与 C-seed 共用暖起点；新运行器 `append_history_r2.py` 延续 A / B-new / C / C-coord / D-nearest / S-segment / S-regroup 的独立历史，R-seed / R-coord 每批均从 default 冷启动。B-new 仅对当前批次从自己的上一计划 local 搜索；C 在累计输入上从自己的上一计划 local 搜索后完整重写。B-mix 的逐组旧 / 新字典选择尚未实现。

为避免误用于主实验，新运行器仅接收最多 **8 MiB** 的清单，标明 functional acceptance，不生成性能结论。它不是大规模可用能力；整批内存、1 GiB 上限、旧包复制和磁盘峰值的差距仍待下一阶段处理。

## 实际检查

### MSVC 既有 Release 二进制

命令：

```powershell
python research/scripts/check_append_history_r2.py --tool build/dictstudy-check/research/tools/Release/seedarc_append4_study.exe --work .sandbox-test/append-history-r2-msvc-final
```

新运行器最终版本实际执行两遍：每遍 **34 个产物、15 次搜索**，全部独立 verify。两遍完整历史与产物 SHA256 相同；B / C / C-coord 的 parent 与 search start 均为本分支上一代；A / B / D / solid-segment 前缀与代数正确，重建路径均一代。夹具含中文名字，输入哈希损坏和超出验收上限均被拒。见 [acceptance-msvc.json](acceptance-msvc.json)、histories-msvc.jsonl / raw-msvc.jsonl。

这里复用已有工具，不声称重新构建了 MSVC / CTest；工具哈希在验收 JSON 中。小夹具只证明调用行为与确定性，不证明候选收益。

### 印度 Linux 固定二进制

按 `in-long-task` 准备 / start 一次，ID `20261123T130743Z-47520bcb`，shared（CPU200%、3 GiB、Swap512 MiB、256 进程），最长 1 小时。只上传两个小型 Python 脚本；只读复用旧任务 `20261123T123105Z-d5596074` 已通过 43/43 的 Release 二进制，没有恢复旧性能任务或改动其完成状态。

实际运行 13:08:23–13:08:27 UTC，exit_code=0，systemd 结果成功，日志未截断；见 [linux-task-result.json](linux-task-result.json)。同一中文夹具两遍各 34 个产物 / 15 次搜索均通过。取回归档 SHA256 `4e0d437467dd1436e0b2bd958c11b66e8347c9a5a38f2208893b0ba39f6b9353`，技能校验通过；路径 `research/local/india-append/artifacts/20261123T130743Z-47520bcb.tar.gz`。独立读取归档后，Linux 与 MSVC 的 34 个历史记录 / 产物哈希逐个相同，见 [cross-platform.json](cross-platform.json)。只对该夹具作此承诺。

验收后已成功 clean 这个新任务的 work/tmp，保留远端结果 / 日志、脚本和本地归档 / 回执。未清理旧中止任务的 unknown 状态现场。当前本轮没有运行中的远端任务。

### GNU Windows 既有二进制（未通过）

补查 `build/g5-gcc/research/tools/seedarc_append4_study.exe`：第一次在中文文件 `fs::relative` / canonical 路径处抛出未处理 filesystem_error，退出码 3221226505；见 gnu-windows-relative.stderr。改为向工具传绝对路径（运行器的输出目录也因此统一为绝对路径）后仍复现，见 gnu-windows-absolute.stderr。没有换英文夹具掩盖失败，没有第三次盲试。

这是既有二进制下真实复现的差异；本轮未重建 GNU 工具，不能确定当前源码 + 当前运行库下的根因，也不能声称 GNU Windows 验收通过。下一次处理时先以当前源码 / 工具链重建并做最小中文路径复现，勿从失败直接推断所有平台的 Unicode 规则有错。

## 搜索空间审查（源码推导 + 旧中止记录的描述性分析）

```powershell
python research/scripts/append_space_audit.py --raw research/local/india-append/interrupted/20261123T123105Z-d5596074/results/raw.jsonl --output research/experiments/20261123-append-r2-readiness-main-001/space-audit.json
```

当前每组有效 D/T 配置为 7 种（D=0 时 T 无意义；其他 3 档各 2 种 T），canonical 的 k=1 固定 rule=0。形式候选数为 **26,902,379**；但 PlanEvaluator 在固定 rule/k 后按组可分，组键表 / Seed 记录开销固定。29 种分组域最多只需 **987 个独立组配置**，即可在原理上构造当前目标的精确最优；等价编码输入遍数上界为 **203 次**，另有字典训练成本。空组、成员集合重复和缓存共享还会减少实际工作。

这不是已实现的精确求解器，也不是“很快就能穷举”的性能结论：大输入下 203 次编码仍可能完全不经济。旧中止 pilot 中 38 次已写完的搜索调用，各自只评估 2–7 个独立组配置；这不能作为全局空间覆盖率。该分析不产生 Seed 价值判定，详见 [space-audit.json](space-audit.json)。

因此不强制先扩字段或改变格式；先量出有限预算下的实际求解成本与资源。只有现有空间不能代表目标时，再引入有现实用途的新耦合维度及修订，不能单靠位宽或人为复杂度制造优势。

## 未完成与下一步

1. 输入 / 训练 / 编码 / 校验的有界处理，以及旧包复制、regroup 多份全量数据和临时产物的资源模型；未抬高 1 GiB 上限，未宣称流式支持。
2. 大规模公开 dev / 新留出清单的版本化与规模梯度、连续追加 / 漂移的候选冻结；尚未构建或读取新的留出集。
3. 在印度测资源与参考求解成本，再选择主实验预算和质量门槛；未启动主性能实验，X/Y/Z/W 没有擅自定值。
4. GNU Windows 中文路径差异待定位；没有修改 C++，没有新的 CLion / Sanitizer / Fuzz 验证，不冒用历史证据。
