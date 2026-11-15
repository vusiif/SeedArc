# ADR-0005：跨平台、固定版本的 Unicode 路径规则

状态：**已实现，待用户验收**。日期：2026-11-14。决定人：用户（2026-11-14 在三个选项中选定：两个平台共用固定版本的表；用简单大小写折叠判断冲突；Unicode 17.0.0）。格式仍为 experimental、未冻结。

输入：[ADR-0001](0001-experimental-container-and-output.md)（路径规则）、[ADR-0004](0004-posix-filesystem.md) §5 / §9（Linux 拒绝非 ASCII，另行立项）、[reproducibility](../reproducibility.md) 第 4 条（文件清单的 Unicode / 大小写处理须确定）。

## 1. 问题

ADR-0001 的路径规则里有两处 Unicode 判断，此前都直接调用 Windows API：

- 路径必须已经是 NFC：`IsNormalizedString(NormalizationC)`；
- 归档清单中，只有大小写不同的名字算冲突：`CompareStringOrdinal(..., ignoreCase = TRUE)`。

这两个 API 的结果取决于 **Windows 内置的 Unicode 版本**：同一个归档在不同版本的 Windows 上，可能一边接受、一边拒绝。这违反了“契约必须版本化、不依赖实现”的底线。Linux 没有对应的系统 API，所以 L1–L3 只实现了 ASCII 子集，用户 2026-11-13 选择暂时拒绝非 ASCII。

## 2. 决定：路径规则 r2（Unicode 部分）

两个平台使用同一份实现（`core/filesystem/unicode_profile.*`）。数据表由 `core/filesystem/unicode_tables_gen.py` 从 [Unicode 17.0.0 数据文件](../../third_party/unicode-ucd/UPSTREAM.md) 生成并提交。不再调用 `IsNormalizedString` / `CompareStringOrdinal`，`libseedarc` 也不再链接 `normaliz`。

一个归档路径（UTF-8 字节串）只有同时满足下面全部条件才被接受。违反第 1–4 条为 `unsafe_path`，违反第 5 条为 `resource_limit`，违反第 6 条为 `unsafe_path`：

1. **严格 UTF-8**（Unicode 表 3-7）：不允许过长编码、代理码位、超出 U+10FFFF 的值，以及缺少或多余的续字节。与此前相同。
2. **每个码位在 Unicode 17.0.0 中已分配**（一般类别不是 Cn）。非字符（如 U+FDD0、U+FFFE）属于 Cn，一律拒绝；私用区（Co）算已分配，可以接受。**这一条是新增的。**
3. **已经是 Unicode 17.0.0 的 NFC**，判定方法：做 UAX #15 的规范分解、规范排序、规范合成后，结果与原文相同。从不悄悄改名。
4. ADR-0001 原有的 ASCII 规则不变：禁止控制字符和 `\ : < > " | ? * ~`，禁止空段、`.`、`..`，禁止以点或空格结尾，禁止 Windows 设备名。
5. 每个路径段最多 **255 个 UTF-8 字节**，整个路径最多 4096 字节，最多 64 段。此前段长按 UTF-16 码元计、上限 255。255 字节正是 Linux 的 `NAME_MAX`，也保证不超过 255 个 UTF-16 码元。纯 ASCII 名字不受影响。
6. **清单冲突**：先对每个码位做简单大小写折叠（`CaseFolding.txt` 中状态为 C 和 S 的映射，包括补充平面；不用 F 和 T），折叠后相同的两条路径算冲突；“某个文件被当作另一条路径的祖先目录”也按折叠后的形式判断。

为什么要求“已分配”：Unicode 的稳定性政策只对**已分配**字符保证 NFC 结果和大小写折叠不再改变。如果允许未分配码位，那么将来升级 Unicode 版本时，同一个名字可能从 NFC 变成非 NFC，或从不冲突变成冲突。要求已分配之后，在 17.0.0 下被接受的路径，在以后的版本里结果也不变。

## 3. 对已有行为的影响

- **Linux**：此前非 ASCII 名字一律返回 `unsupported_feature`，现在按上述规则接受或拒绝，结果与 Windows 相同。压缩和解压都直接使用已验证的 UTF-8 原字节，名字必须已是 NFC，所以写到磁盘上的名字就是归档里存的那串字节。
- **Windows 会变严**，具体有以下几处（数字来自在本机 Windows 10 19045 上运行的 `research/tools/unicode_win_diff.cpp`，见 [验证记录](../unicode-path-validation.md)）：
  - 未分配码位此前会被接受，现在被拒绝。本机 Windows 把 814,664 个未分配码位都判为 NFC。
  - 冲突判断变严：有 539 个码位按新规则会与另一个码位冲突，而 `CompareStringOrdinal` 认为它们不同，例如 U+00B5 µ / U+03BC μ、U+017F ſ / s、标题大小写的双字母（如 ǅ / ǆ）、补充平面的大小写对。反方向只测了每个 BMP 码位与其 Windows 大写形式（`CharUpperBuffW`）组成的配对：Windows 认为相同而新规则不认为相同的，0 对。任意两个码位之间的全部配对没有穷举。
  - NFC 判断：对每个已分配码位单独判断，与 `IsNormalizedString` 0 处不同。码位后接 U+0301 时，有 3 处不同（U+1AEB、U+1E4EC、U+1E4ED）。这些字符比本机 Windows 的 Unicode 版本更新，Windows 把它们当作组合类为 0 的字符处理，按 17.0.0 判定的才是正确结果。私用区码位后接 U+0301 时还有 850 处不同，原因是 Windows 自己对私用区的处理，按 UCD 私用区字符没有分解。
  - 非 ASCII 的长名字：段长超过 255 个 UTF-8 字节就会被拒绝（例如超过 85 个汉字）。
- **已有归档**：M3 固定样例都是 ASCII 名字，不受影响。格式是 experimental，没有对外承诺。之前在 Windows 上生成、名字落在上述变严范围内的归档，现在解压时会被拒绝。这是有意的取舍，记录在此。
- **归档字节布局不变**，归档头里也没有记录路径规则修订号。如果将来冻结格式，规则修订必须一起写进契约。

## 4. 版本与演进

- 当前是“路径规则 r2 = Unicode 17.0.0”。改用新的 Unicode 版本，或调整上面任何一条，都是新的规则修订：新数据放进新的 `third_party/unicode-ucd/<version>/` 目录，写新 ADR，并说明兼容性。不能静默替换（AGENTS.md 第 6 节）。
- 表是生成后提交的文件，不在构建时联网下载。`unicode.tables_current` 测试确认已提交的表与固定数据一致。

## 5. 没有选的方案

- **只给 Linux 单独做一套表、Windows 继续用系统 API**：同一归档在两边可能结果不同，并随 Windows 版本漂移。
- **在 Linux 上引入 ICU**：依赖很大，而且 ICU 的 Unicode 版本同样与 Windows 不同，不能解决一致性问题。
- **按 Windows 的方式比较大小写（每个 UTF-16 码元做简单大写）**：不对称，补充平面不折叠，也不是标准做法。解压时 NTFS 自身不区分大小写的“不覆盖”检查仍然有效，与这里的选择无关。

## 6. 验证

见 [Unicode 路径验证记录](../unicode-path-validation.md)，包括：`NormalizationTest.txt` 全量一致性测试、表与数据一致的检查、路径与清单的边界测试、两个平台上的 Unicode 往返与拒绝测试，以及与 Win32 的差分结果。
