# Unicode Character Database（固定版本）

| 字段 | 内容 |
| --- | --- |
| 名称与用途 | Unicode Character Database（UCD）的部分数据文件。归档路径规则（[ADR-0005](../../docs/decisions/0005-portable-unicode-path-profile.md)）里的 NFC 判定、已分配码位判定和简单大小写折叠，都从同一个固定版本生成，Windows 和 Linux 因此得到相同结果。`NormalizationTest.txt` 只给一致性测试用。 |
| 来源 | 官方发布目录 `https://www.unicode.org/Public/17.0.0/ucd/`；许可来自 `https://www.unicode.org/license.txt`。2026-11-14 用 `curl --proto =https` 获取，用户事先同意了这次下载。 |
| 固定版本 | Unicode 17.0.0（每个文件首行和 `ReadMe.txt` 都写明 17.0.0）。SHA256 见下表。 |
| 许可与版权 | Unicode License v3，原文为 [17.0.0/license.txt](17.0.0/license.txt)（“Copyright © 1991-2026 Unicode, Inc.”）。数据文件保留了原始声明。项目自身的 GPL-3.0-or-later 不取代该许可。 |
| 本地修改 | 无。文件按原字节保存：`.gitattributes` 里 `/third_party/unicode-ucd/** -text`，检出时不做换行转换。 |
| 构建集成 | 不参与编译。`core/filesystem/unicode_tables_gen.py`（只用标准库）从这里生成 `core/filesystem/unicode_tables.inc`（已提交），生成结果的文件头里记录各输入文件的 SHA256；该 `.inc` 只被 `core/filesystem/unicode_profile.cpp` 包含。 |
| 验证 | `unicode.conformance`：用 `NormalizationTest.txt` 的全部用例检验 NFC，并对 Part 1 以外所有码位检验 NFC 不变。`unicode.tables_current`：用生成器的 `--check` 确认已提交的表与这些数据一致（需要 Python 3，缺少时登记为 DISABLED，而不是悄悄省略）。实测结果见 [Unicode 路径验证记录](../../docs/unicode-path-validation.md)。 |
| 更新方法 | 新版本放进新目录 `unicode-ucd/<version>/`，不覆盖旧目录；更新生成器里的 `version` 后重新生成 `.inc` 并跑全部测试。路径规则改用新版本属于新的规则修订（ADR-0005 §4），不能静默替换。 |

## 文件与 SHA256

| 文件 | 字节 | SHA256 |
| --- | --- | --- |
| `17.0.0/ReadMe.txt` | 740 | `9fe1a90bd32659d7953616283dc2bffaa165518aae9ace026040c42c559ba606` |
| `17.0.0/UnicodeData.txt` | 2,198,209 | `2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c` |
| `17.0.0/DerivedNormalizationProps.txt` | 1,377,582 | `71fd6a206a2c0cdd41feb6b7f656aa31091db45e9cedc926985d718397f9e488` |
| `17.0.0/CompositionExclusions.txt` | 9,007 | `2f239196ef3b5b61db5cc476e9bd80f534d15aa1b74e1be1dea5d042a344c85f` |
| `17.0.0/CaseFolding.txt` | 87,539 | `ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183` |
| `17.0.0/NormalizationTest.txt` | 2,827,429 | `5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db` |
| `17.0.0/license.txt` | 1,995 | `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96` |

`CompositionExclusions.txt` 没有被生成器读取（排除集取自 `DerivedNormalizationProps.txt` 的 `Full_Composition_Exclusion`），只作为人工核对时的参考保留。Unicode 官方不发布校验和，上表是下载后在本机计算的。
