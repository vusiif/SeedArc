# g5-holdout-r1

G5（DEV-016）的**一次性正式评估集**，不用于任何调参。计划见 [seed-g5-r1](../ideas/seed-g5-r1.md)。清单与环境见 [JSON](g5-holdout-r1.json)，建库和检查入口是 `research/scripts/g5_holdout.py`。用户 2026-11-14 选定“本机 + 公开”混合来源，并同意下载 Django 6.1.3 源码包。

## 组成（2026-11-14 构建）

| 数据池 | 一半 | 来源 | 文件 | 字节 | 组（取用 / 可用） | 中位文件大小 |
| --- | --- | --- | --- | --- | --- | --- |
| many-small-docs（每个文件 ≤ 32 KiB） | local | MSYS2 `ucrt64/share/doc` | 178 | 1,189,588 | 7 / 8 | 3,506 |
| | public | Django 6.1.3 `docs/`（.txt，即 reStructuredText） | 505 | 2,049,983 | 11 / 13 | 1,524 |
| source-python | local | Python 3.15 `site-packages`（第三方包，不含测试目录） | 345 | 2,729,612 | 8 / 11 | 3,863 |
| | public | Django 6.1.3 `django/`（不含测试目录） | 410 | 3,142,483 | 26 / 51 | 3,695 |
| config-cmake-pkg | local | VS Build Tools MSBuild `VC/v180` 的 `.props` / `.targets` / `.xml`；WSL Ubuntu 软件包安装的 `x86_64-linux-gnu/{cmake,pkgconfig}` 和 `share/pkgconfig` | 241 | 1,244,467 | 163 / 163 | 1,435 |
| | public | vcpkg `fe8e78fa` 的 `ports/`（cmake / json / cmake.in / usage） | 1,000 | 1,051,771 | 423 / 2,919 | 620 |

合计 2,679 个文件、11,407,904 字节、2,673 种内容。

## 规则

- **与 r2 不相交**：内容 SHA256 出现在 [b2r5-corpus-r2](b2r5-corpus-r2.md) 任一清单（dev 或 test，共 4,724 种内容）中的文件一律排除。`--check` 从已提交的清单重新核对，结果：共享内容 0。
- **路径**：每个语料路径都用 `seedarc_path_check`（直接调用 `libseedarc` 的 `validate_archive_path`，即 ADR-0005 规则）检查过，0 个被拒。
- **排除项**：空文件（source-python 196 个，config 1 个）；符号链接（WSL 来源在 WSL 内部用 `find -type f` 列出普通文件作白名单，因为经 `\\wsl.localhost` 访问时符号链接不能可靠识别）；文档池中超过 32 KiB 的文件 92 个；测试目录。
- **选择**：每个池的每一半，按 `sha256("g5-holdout-r1:<组>")` 的顺序取整组，直到字节或文件上限（docs、config 每一半 2 MiB / 1,000 个文件；Python 每一半 3 MiB / 1,000 个文件）。整组要么全收，要么不收。
- **不收的来源**：WSL `/etc`，因为含本机特有信息，如用户名；WSL `/usr/share/cmake-4.2`，因为是 CMake 本身，与 r2 同一项目。

## 存放与许可

- 原始文件只放在被 Git 忽略的 `research/local/g5-holdout-r1/files/`。Django 源码包在 `downloads/`，解包结果在 `raw/`。仓库里只有相对路径、大小、SHA256 和分组。
- 本机一半（MSYS2 文档、site-packages、MSBuild 文件、Ubuntu 软件包配置）许可按各软件包或 Microsoft 的条款，**只在本机评估，不再分发**。本机软件版本变化后，不能假定能逐字节重建。
- 公开一半可以复现：Django 6.1.3 sdist（`django-6.1.3.tar.gz`，11,306,257 字节，SHA256 `b5d2d9e35295065d13b1927b3f19a59bd119f737aa6299def14ccca1ae3c8c0d`，与 PyPI 一致，BSD-3-Clause；用 `tarfile` 的 `data` 过滤器解包，包内无链接）；microsoft/vcpkg 提交 `fe8e78fa` 的 `ports/`（MIT）。

## 使用记录

- 尚未使用。G5 的参数在 r2 dev 上冻结之后，才在这里**评估一次**。

## 构建过程中的修正（发生在任何测量之前）

1. 工具路径相对，`CreateProcess` 找不到；改为先解析成绝对路径。这一次没有写出任何文件。
2. 经 `\\wsl.localhost` 读取 WSL 里的符号链接 `libcrypt.pc` 时报错；改为在 WSL 内部列白名单。
3. `find -printf '%P\0'` 里的反斜杠经 `wsl.exe` 传参后丢失，分隔符变成字符 `0`，导致所有 WSL 来源都是 0 个文件；改用 `-print0`，再去掉前缀。
4. 文档池加上每个文件 32 KiB 的上限，与 r2 “many small files”的定义对齐（r2 test 文档平均约 1.8 KB）。

前三次失败或不完整的输出（未提交）都已删除后重建，本文和 JSON 是第四次构建的结果。
