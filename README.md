# SeedArc

Seed-Driven Adaptive Compression：使用 C++23 开发的实验性文件与目录混合压缩项目。

基于成熟压缩库，通过版本化种子映射生成策略，研究在有限预算内能否缩小**完整归档**。种子不是文件内容或加密密钥，压缩率收益尚未实测。

## 状态与文档

**当前进度以 [STATUS.md](STATUS.md) 为准**，本节只做简述。目前已有工程骨架、实验性归档容器和 Windows 安全输入 / 输出；CLI 提供 `compress`、`verify`、`extract`，每个分块可用 Store（不压缩）、Zstandard 或 LZMA2 编码（`--codec`、`--level`，压不小的块自动按 Store 保存），可逐字节往返。种子与搜索尚未实现，归档格式为实验性且未冻结（不要用它保存唯一的数据副本）。

在项目根目录使用 MSVC / VS 2026 构建：

```powershell
cmake --preset msvc-vs2026-debug
cmake --build --preset msvc-vs2026-debug --parallel
ctest --preset msvc-vs2026-debug
./build/msvc-vs2026-debug/cli/Debug/seedarc.exe --help
```

```powershell
seedarc compress <文件或目录> -o <归档>
seedarc verify <归档>
seedarc extract <归档> -o <新目录>
```

输出从不覆盖：归档和解压目录必须尚不存在，其父目录需位于本地 NTFS 卷。输入中的符号链接 / junction 会被拒绝。失败或取消不会留下半成品；仅当进程被强杀时，输出旁可能残留名为 `.seedarc-<hex>.tmp` 的临时文件或目录，可直接删除。退出码与限制见 `seedarc --help` 和 [ADR-0002](docs/decisions/0002-m2-store-archive-constraints.md)。

Release 将上述 preset 名替换为 `msvc-vs2026-release`，可执行文件位于对应目录的 `cli/Release/`。GCC / Clang 用法及检查命令见[开发环境](docs/development.md)，各里程碑验证记录由 STATUS 链接。

- [开发计划](docs/development-plan.md)
- [当前进度与协作交接](STATUS.md)
- [研究归档](research/README.md)：探索思路、配置、数据集清单、实验记录、基准和分析脚本。
- [第三方源码](third_party/README.md)：来源、版本、许可与本地修改约定。
- [架构](docs/architecture.md)与[开发环境](docs/development.md)
- [归档要求](docs/archive-format.md)与[确定性](docs/reproducibility.md)
- [安全](docs/memory-safety.md)与[测试](docs/testing.md)
- [Agent 约定](AGENTS.md)与[执行流程](docs/agent-workflow.md)
- [用户想法](docs/users_idea/)：开发时检查更新并评估，包含[历史构想](docs/users_idea/SeedPack_项目设计方案_v2.md)和[结构化种子草案](docs/users_idea/SeedArc_结构化种子设计草案.md)。当前统一命名为 SeedArc。

远程仓库：<https://github.com/vusiif/SeedArc>。

## 许可证

SPDX-License-Identifier: GPL-3.0-or-later

SeedArc is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

SeedArc is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
SeedArc. If not, see <https://www.gnu.org/licenses/>.

完整正文见 [LICENSE](LICENSE)。第三方依赖保留各自许可证与版权声明，引入时单独核对。
