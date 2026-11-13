# B2-r2 文件身份与叠加契约观察

日期：2026-11-12（America/Los_Angeles）。负责人：主 Agent。状态：计划已预先登记，现已运行完成，行为验证通过。

问题：文件列表改变时能否避免覆盖误绑定；叠加编辑清除是否回到当前默认；空文件、无目标 Codec、大文件是否可解释。

预设方法：独立 catalog-r1 将精确规范路径映射到本次文件 ID，复用既有 Windows NFC UTF-8 路径验证；旧路径不匹配即失败。保持 overlay-r1 与 A/B-r1 不变。固定三文件十二块，比较 base / global / file / cleared；插入、移除、重排、重编号、重命名另行测试。增加显式空文件、单块无目标 Codec、128 块文件观察与 10000 块单测。

预期：目标文件 Zstd 3 → 4 → 6 → 4，其他文件跟随全局4；覆盖不更改 Codec。列表变化覆盖只绑定同路径，未知路径明确失败。清除为删除条目重新计算。Global ID 仍决定 Codec，不承诺重新编号后的 Codec 稳定。

预算：有界纯计划测试（最多65536块、4096文件 / 覆盖），不读取用户文件、不压缩、不搜索；不运行 GUI 或 Actions。不以 Debug 耗时或单次运行评价性能。

验证计划：Release 全量、ASan / Clang 定向、新工具跨编译器输出、CLion 完整 warnings；实验保存源码快照、配置、原始输出及分析脚本。压缩率、真实搜索与内存尚未测量。

## 实际执行与结果

代码基线 f3deb4a15305760e4714136b93c066071ad87b02，加未提交的 overlay-r1 与本轮 catalog-r1。source/ 保存全部 seed 源码及相关未跟踪工具 / 测试 / CMake，source-sha256.json 核对快照；依赖与生产 core 沿用该 Git 基线。旧实验原件不修改。配置、输入逻辑清单及所有块身份 / 四阶段计划见 config.json、observation.json。

实际命令：

```powershell
cmake --preset msvc-vs2026-release -DSEEDARC_BUILD_RESEARCH_TOOLS=ON
cmake --build build/msvc-vs2026-release --config Release
ctest --test-dir build/msvc-vs2026-release -C Release --output-on-failure
./build/msvc-vs2026-release/research/tools/Release/seedarc_seed_scope_contract.exe --observe
python research/scripts/file_contract_table.py research/experiments/20261112-file-contract-main-001/observation.json
```

前两条为已有 preset 的复现配置与本轮实际增量构建；本轮没有重复 configure 命令，现有缓存保持研究工具 ON。原始 stdout 通过 Python subprocess.run(capture_output=True) 按字节保存而非终端转码。实际 ASan / Clang 及 IDE 检查范围见 [验证记录](../../../docs/file-contract-validation.md)，logs/ 保留所有执行结果、初轮受限失败与正常用户重建证据。

Release 全量34/34（51.70s）；IDE风格修正后Release定向4/4（0.12s）。ASan定向4/4（1.21s），最终新测试1/1（0.53s）；Clang Debug定向3/3（0.28s）。MSVC Release与Clang Debug的86434字节观察输出一致，工具CLI边界4/4。CLion真实Debug配置重新加载成功，4个新增C++与3个CMake文件完整诊断无WARN/ERROR；IDE新测试、工具均退出0。

[可读对照](preview.md)：组合编辑中目标 Zstd 块5/6为3→4→6→4；其他Zstd为3→4→4→4。插入重排保留ID时原12块不变；重建ID后仅同路径的目标Zstd块105受覆盖，Codec允许改变。空文件与无Zstd单块文件成功且无变化；128块大文件41个目标块变化。删除 / 重命名后的旧键明确 invalid_argument；10000块单文件作用域及其他错误边界由新单测验证。

## 限制与下一步

这是一次合成纯计划观察，不读取实际文件、不压缩 / 解压、不搜索、不创建种子格式；没有完整归档体积、生成性能或峰值内存测量。未运行持续Fuzz、完整ASan矩阵、非Windows平台。不能据此判断压缩收益、搜索效率或真实大文件的最佳颗粒度。最终Seed编码与文件身份方案仍未冻结；下一步先评估此契约，再选择真实数据和公平基线。
