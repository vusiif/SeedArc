# B2-r3 真实压缩首轮

日期：2026-11-12（America/Los_Angeles）。任务DEV-008，主Agent。状态：方法预先登记，现已完成真实压缩首轮，功能与小型基线通过。

问题：B2文件级覆盖能否在既有安全压缩管线中生成可独立解码归档；在相同Codec分配下修改等级有什么实际影响。功能正确性与压缩收益分别报告。

方案：生产core只接收具体Codec/config计划，在压缩器实际安全枚举之后调用内部计划构建入口；研究层解释B-r1与catalog-r1，不更改原映射或归档布局。沿用每块实际Codec/config索引、Store回退、自校验及事务提交。未知策略版本、非法计划、取消失败不得发布归档。解码不重放计划或搜索。

预设对照：固定Store、Zstd-3、Zstd-6、LZMA2-6，以及B-r1相同codec_seed=0下base(Zstd3)、global(Zstd4)、file(global4+目标Zstd6)、cleared(global4)。LZMA2均preset6。固定块大小64KiB，单线程，无搜索，候选数1。至少3次重复，轮转方案顺序；时间含进程启动与完整性自检，分开记录compress/verify/extract。测量Windows进程峰值工作集，不能替代系统总内存或单纯codec占用。

输入：实际写入磁盘的项目自制文本、源码形态、二进制、gzip已压缩数据及混合目录/空文件；固定生成器、文件清单与SHA256，不代表独立真实语料评估。首次用于链路与可复现基线，不以此判断代表性收益。大型输入/归档/解压输出保留research/local本run目录。

预算：小型输入，各操作120s上限。测试包括新接入与现有回归、旧归档兼容、计划/实际codec差异、同环境字节一致、ASan与Clang定向；集成完成后集中CLion诊断。Actions与GUI继续延后。

## 实际代码与复现

基线提交 f3deb4a15305760e4714136b93c066071ad87b02，加未提交的B2-r1/r2与本轮改动。source/保存相关源码、测试、工具与CMake，source-sha256.json核对；tracked-source.patch记录受跟踪文件差异。dataset-source/保存数据集采用的三份项目源码原件，datasets.json绑定内容哈希。依赖沿用vcpkg锁定版本；配置中记录工具SHA256、Python/OS/CPU、单线程与时间/内存口径。

```powershell
# 清洁检出复现时先覆盖source/，已有本轮构建缓存无需重复configure：
cmake --preset msvc-vs2026-release -DSEEDARC_BUILD_RESEARCH_TOOLS=ON
cmake --build build/msvc-vs2026-release --config Release
ctest --test-dir build/msvc-vs2026-release -C Release --output-on-failure
# 本轮实际基线命令：
python research/scripts/b2_compression.py --tool build/msvc-vs2026-release/research/tools/Release/seedarc_seed_archive.exe --run-id 20261112-b2-compression-main-001 --repetitions 3
# 只读原始结果生成表：
python research/scripts/b2_report.py research/experiments/20261112-b2-compression-main-001/raw.json
```

实际各次compress/verify/extract命令、exit状态、时间与峰值工作集见raw.json与360份操作日志；初始两次构建/测试失败日志与最终修复后的构建、CTest、IDE记录都保存在logs/。正常用户身份运行文件系统测试，避免既有沙盒ACL / WindowsApps pwsh限制。基线在构建与测试结束后单独运行。

## 功能证据

Release35/35（52.38s），ASan全量35/35（62.12s），Clang Debug定向5/5（0.62s）。IDE修正后Release定向8/8（6.37s），ASan新集成1/1（0.26s）、Clang新集成1/1（0.24s）。六个C++与三个CMake文件最终完整IDE诊断无ERROR，保留两个既有公开接口stop_token按值传递建议，新增内部转发已改const引用；IDE新测试与工具运行退出0。详见 [验证记录](../../../docs/b2-compression-validation.md)。

双向兼容：固定SHA256的M3旧CLI验证/提取本轮8种混合目录归档；当前程序验证/提取3个旧样例，内容逐字节准确，见compatibility.json。cross-toolchain.json保存5类b-file的MSVC Release / Clang Debug实际计划、块描述与归档一致观察。determinism.json另行按字节比较40组重复归档以及15组cleared/global归档，均一致；不只依据SHA256。CLI边界7项通过，另有Unicode归档文件名与目标键成功，见cli-smoke.json。

## 基线结果与限制

5类×8方案×3次，共120/120磁盘往返逐字节还原。每个压缩结果保存真实规范文件清单、拟议Codec/config、实际Codec/config与存储长度；无收益Store回退明确区分。完整归档大小、压缩/验证/解压时间与峰值工作集见raw.json、summary.json，[可读表](preview.md)由分析脚本生成。

相同Codec分配下文件覆盖相对全局：源码样本减少667字节，binary/压缩数据/mixed/text四类不变。全局B-r1含固定Store分配，部分输入的单算法参考明显更小；不掩盖此结果。清除继承当前全局，归档字节一致。

磁盘实际文件不等于代表性真实语料。本轮源码来自本项目，其余是自制小样本；没有独立评估集、搜索或调参预算。时间包含启动、自检与证据序列化，峰值工作集包含工具/报告，不是纯codec内存。无预热、OS缓存不受控，3次重复不足以推断统计显著性；不做压缩率/效率普遍结论。原始Seed/覆盖未序列化，不能评估最终Seed元数据成本。未新跑持续Fuzz、UBSan、非Windows平台。

下一步选择获授权的代表性数据，并评估固定Codec分配中Store占比的代价；Seed编码/文件身份/映射仍可继续探索。GUI与Actions继续延后，本轮未提交推送。
