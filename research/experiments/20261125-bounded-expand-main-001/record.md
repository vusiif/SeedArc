# 有界固实扩展 dev：源码 / 文本分层与 10% / 40% 新批次

2026-11-25，DEV-023，主Agent（Claude），无委派。状态：执行者报告通过，**尚未经主模型独立验收**。按[测试任务书](../../../docs/test-execution-plan.md)执行；不改 core、协议、Seed 语义、C++ 压缩实现或默认参数。用户本轮授权使用印度服务器全部计算资源（独占模式）；skill 的 2 GiB 任务磁盘监控保留。

## 阶段 A：复算 DEV-022

起点 `d03cdaaad5e4dca56e6789c6969f9603145c3baa`，工作区既有残留（`.sandbox-test/`、`docs/debug.log`、未提交的 HANDOFF / STATUS / 任务书）保留未动。两份 users_idea 原稿 SHA256 与 STATUS 已读快照一致。

按任务书命令复算，输出在 [recomputed.json](recomputed.json) / [recomputed.md](recomputed.md)：120 命令、44 归档的断言全部通过；与原 analysis.json 逐值比较 0 处差异，预览表逐行相同。见 [stage-a-check.json](stage-a-check.json)。

## 阶段 B：预登记

在任何 DEV-023 压缩命令之前保存：

- 数据：[bounded-expand-dev-r1](../../datasets/bounded-expand-dev-r1.md)，规则写在 [bounded_expand_manifest.py](../../scripts/bounded_expand_manifest.py)；
- 配置：[bounded-expand-dev-r1.json](../../configs/bounded-expand-dev-r1.json)。

| 类 | 文件 | 字节 | 最大文件 | added10 实际 | added40 实际 |
| --- | ---: | ---: | ---: | ---: | ---: |
| source（Linux 6.12 源码扩展名） | 11,285 | 268,433,674 | 16,414,003 | 9.999% | 39.997% |
| text（enwik8 dev 切段 + Silesia nci） | 49 | 81,553,445 | 33,553,445 | 9.810% | 40.464% |

text 只有 77.8 MiB，低于 128–256 MiB 目标。按任务书使用全部合格文件，不填充。文本规则在测量前已经固定：
- 排除 2 个 enwik8 切段：无效 UTF-8 序列超过 4 个；
- 排除 Silesia 的 reymont、samba、sao、xml：都含 NUL，其中 xml 是 tar 文件。
看到排除结果后没有改规则。

矩阵：2 类 × 2 批次 × 4 帧档（1 GiB 参考 / 32 / 64 / 128 MiB）× 4 轮 × 2 操作，共 128 次主编码。帧档四轮循环移位，各占一次第 1–4 位；重建 / 追加的先后逐轮交替。

每个单元另有：
- 父包准备：每档一次，含 Seed 打包 + verify、显式打包 + verify、payload-compare，单独计时；
- 每个输出：verify 和读取 / 解码诊断；
- 每个帧档 / 操作的首个输出做一次载荷故障诊断。

每单元 124 条命令，合计 496 条。

## 运行器与 smoke

新增独立脚本，旧 DEV-022 运行器和断言未动：
- [bounded_expand_dev.py](../../scripts/bounded_expand_dev.py)：运行器；
- [bounded_expand_task.py](../../scripts/bounded_expand_task.py)：远端任务；
- [bounded_expand_report.py](../../scripts/bounded_expand_report.py)：独立审计 / 分析；
- [bounded_expand_preview.py](../../scripts/bounded_expand_preview.py)：表格；
- [bounded_expand_smoke.py](../../scripts/bounded_expand_smoke.py)：smoke。

最终树直接使用数据目录，加载时检查目录里没有清单之外的文件，单元结束后再全量核对一遍。

Smoke 用固定小树：空文件、中文目录 / 文件名、24 个小文件、一个恰等于 4096 B 帧上限的文件。
- 两个批次各 124 命令 / 40 包，全部逐字节 verify，重复 SHA 稳定，审计通过。
- 4097 B 超上限文件：退出码 1，stderr 为 `file exceeds the requested solid frame limit`，没有输出文件，stdout 为空。
- Windows（MSVC Release）与 Linux（GCC 14 Release）的 smoke 归档 SHA 全部一致。

## 远端执行

印度 `vps-in1`，任务 `20261125T154526Z-8e750889`，独占模式，测量子进程固定单 CPU。
- 运行时间：UTC 2026-11-25 15:46:33 – 16:26:11，exit 0；
- 日志 56,554 B 全部保留，无截断；
- 上传 2,798,153 B，10 个输入文件 SHA 与 [input-hashes.json](input-hashes.json) 一致。

磁盘监控补丁只改本任务的 runner 副本，与 DEV-022 同源：原 SHA `4118a90c…`，补丁后 `d88dc2fc…`。已安装的 skill 未改，见 [monitor-patch.json](monitor-patch.json) / [task-monitor-patch.py](task-monitor-patch.py)。

构建与检查：
- 固定 GCC 14.2 / vcpkg 状态 SHA 核对通过；
- Release 构建 0 warning / error，CTest 46/46；
- 13 份固定小包的跨平台 SHA 与 DEV-022 参考一致；
- 生成的 ELF SHA `435ea5b4…` 与 DEV-022 / DEV-021 完全相同；
- 源码 zip 的 135 个文件与 DEV-022 源码哈希逐一相同。
- Sanitizer 本轮未运行：C++ 没有变化，沿用 DEV-021 相关 ASan 4/4，不冒充新覆盖。

结果：
- 496 条命令全部成功，四个单元各自通过独立审计。审计内容：命令数、位置平衡、操作交替、所有 verify 逐字节、帧上限、物理大小、追加前缀与旧包读写量、旧原文重压 0、父包不变、重复 SHA 稳定、Seed / 显式载荷相同、采样集合一致、故障诊断接受错误字节 0。
- 同类两种批次的全量重建 SHA 完全一致（最终树相同）。
- bundle `aee92348d0a4b84bd4bff30f4e16a2e7bf3879e3f77c54fdf61aa170df653f07` fetch 核验通过。本机重新运行审计，得到的 analysis.json 与远端逐字节相同。随后 clean 了 work/tmp，保留远端 results / logs。
- 资源：编码 RSS 峰值 824 MB，读取诊断峰值 581 MB；命令前后逻辑磁盘峰值 897,943,991 B，不是连续瞬时峰值。全部用时 2,377 s，预算 90 分钟。

## 观测（dev、热缓存、单机单 CPU）

完整表见 [preview.md](preview.md)，机器数据见 [analysis.json](analysis.json)，原始数据见 [remote/](remote/)。时间为四轮最小 / 中位 / 最大。"读取 / 解码诊断"包含整包读取、目标帧解码与字节校验，不写出文件，不是真实提取耗时。

| 类 | 帧上限 | 全量重建包 B | 比整段 | 编码 s 中位 | 读取诊断中位 ms | 同轮读取倍率 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| source | 整段 | 28,141,155 | — | 11.03 / 11.23 | 496 / 482 | 1.00× |
| source | 32 MiB | 28,410,983 | +0.959% | 10.49 / 10.82 | 164 / 153 | 2.65–3.31 / 2.72–3.44× |
| source | 64 MiB | 28,301,832 | +0.571% | 10.77 / 10.89 | 211 / 215 | 2.15–2.38 / 2.07–2.31× |
| source | 128 MiB | 28,279,054 | +0.490% | 10.92 / 10.78 | 296 / 299 | 1.57–1.73 / 1.42–1.66× |
| text | 整段 | 17,006,369 | — | 4.41 / 4.67 | 204 / 203 | 1.00× |
| text | 32 MiB | 17,065,581 | +0.348% | 4.37 / 4.66 | 94 / 93 | 1.93–2.25 / 1.91–2.78× |
| text | 64 MiB | 17,003,161 | −0.019% | 4.21 / 4.51 | 149 / 163 | 1.26–1.44 / 1.16–1.53× |
| text | 128 MiB | 17,006,369（与整段同 SHA） | 0 | 4.50 / 4.66 | 212 / 214 | 0.87–1.06 / 0.90–1.04× |

斜杠前后分别是 added10 / added40 单元的重复测量；同一最终树的两组输出包字节相同。

**追加对全量重建**（每档父包固定，只追加一次）：

| 类 / 批次 | 追加 / 重建墙钟 | 追加后比同档重建大 | 复制旧包 B（整段档） |
| --- | --- | --- | ---: |
| source 10% | 0.31–0.41 | +3.67% – +3.86% | 26,775,553 |
| source 40% | 0.53–0.62 | +6.27% – +6.73% | 19,034,314 |
| text 10% | 0.26–0.31 | +0.34% – +0.41% | 14,557,584 |
| text 40% | 0.57–0.69 | +0.10% – +0.48% | 6,852,555 |

旧原文重压量全部为 0，但每次都读取并复制整个旧包，再完整写出新包。父包准备另列，源码 10% 每档约 9.8–10.6 s，不计入单次追加。只有一次新增批次，不能代表多代生命周期。

**故障诊断**：32 次载荷故障诊断都没有接受错误字节（wrong_bytes_accepted = 0）。每次在压缩载荷的 10 / 50 / 90% 处各翻转 1 bit。不同布局下这几个位置落在不同的帧和文件上，所以每档只有 3 个点，不足以按恢复能力给帧档排序。诊断以头部和索引完好为前提，不代表索引损坏、缺卷或通用恢复。

## 解读与限制

1. **源码类代价比 mixed dev 大**：32 / 64 / 128 MiB 分别 +0.96 / +0.57 / +0.49%，DEV-022 mixed dev 为 +0.47 / +0.05 / +0.04%。源码更依赖长距离匹配。读取诊断收益在两种批次上都复现：32 MiB 约 3×、64 MiB 约 2.2×。
2. **text 64 MiB 重建**：包比整段小 0.019%，读取诊断约快 1.3×，编码时间相近。在本 dev 中可标为对整段的支配点，但只是一类、一次布局、77.8 MiB 小树的观察。
3. **噪声带**：text 128 MiB 与整段字节相同，它的同轮读取倍率 0.87–1.61× 就是本机四轮的测量噪声。小于这个范围的读取差异不应解读。text 40% 追加的 32 / 64 MiB 读取倍率（0.96–1.72×）落在噪声带内，不作为收益。
4. **追加**：用一次复制旧包的 I/O 换来编码时间 0.26–0.69 倍。代价是追加包比同档重建大：源码最多 +6.7%，文本最多 +0.5%。没有测磁盘读写量，逻辑包大小不能当作实际 I/O。
5. **编码时间**：各档编码时间相近（源码约 10.5–11.2 s），没有压缩吞吐上的突破。
6. **适用范围**：只有一台机器、热缓存、四轮，不构成统计显著性；没有 holdout，不能作跨数据泛化；Seed 与显式载荷相同，收益来自布局，不归因于 Seed。
7. **不设默认**：按用户"由用户选择取舍"的方向，不设通用默认、不冻结预设、不改研究选项表。是否把 text 64 MiB 的支配观察纳入选项，留给主模型审计后再定。

后续仍按任务书另立：真实落盘提取与 I/O 分解、选择性磁盘读取、多代追加 / 整理生命周期、多 codec 同时间曲线、索引损坏 / 缺卷恢复、偏好接口。

## 复现

```text
python -B research/scripts/bounded_expand_manifest.py --linux-manifest research/datasets/append-linux-large-dev-r2/dev-linux-512.tsv --wiki-dir research/local/append-r1/files/dev/wiki-text --silesia-dir research/local/append-r1/files/dev/silesia/silesia --mixed-manifest research/datasets/solid-mixed-version-dev-r1/v0.tsv --path-tool <seedarc_path_check> --output <new-dir>
python -B research/scripts/bounded_expand_smoke.py <seedarc_append4_study> <new-dir>
python -B research/scripts/bounded_expand_report.py research/experiments/20261125-bounded-expand-main-001/remote <analysis.json>
python -B research/scripts/bounded_expand_preview.py <analysis.json> <preview.md>
```

远端任务入口为 `task.sh` → `bounded_expand_task.py`，输入清单见 input-hashes.json。完整 bundle 和回执在 `research/local/india-append/artifacts/20261125T154526Z-8e750889.*`，该目录被 git 忽略。[peek_progress.py](peek_progress.py) 是运行期间只读查看远端进度的辅助脚本。

尚未提交 / 推送。

## 后续更正（2026-11-25，DEV-024）

上文"解读与限制"第 2 条所说的文本 64 MiB 支配点，只对读取 / 解码诊断成立。在 DEV-024 的真实索引式提取中（只解码到目标文件为止），文本 64 MiB 对整段只有 0.93–1.10×，落在噪声带内，见 [DEV-024 记录](../20261125-bounded-extract-main-001/record.md)。本文其余事实不变。
