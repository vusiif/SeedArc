# SeedArc

Seed-Driven Adaptive Compression：使用 C++23 开发的实验性文件与目录混合压缩项目。

基于成熟压缩库，通过版本化种子映射生成策略，研究在有限预算内能否缩小**完整归档**。种子不是文件内容或加密密钥，压缩率收益尚未实测。

## 状态与文档

当前已完成 M0 工程骨架：C++23 核心库、CLI、CMake presets 与 CTest。CLI 目前仅支持帮助与版本；压缩、解压、校验及搜索尚未实现，归档格式未冻结。

在项目根目录使用 MSVC / VS 2026 构建：

```powershell
cmake --preset msvc-vs2026-debug
cmake --build --preset msvc-vs2026-debug --parallel
ctest --preset msvc-vs2026-debug
./build/msvc-vs2026-debug/cli/Debug/seedarc.exe --help
```

Release 将上述 preset 名替换为 `msvc-vs2026-release`，可执行文件位于对应目录的 `cli/Release/`。GCC / Clang 用法及检查命令见[开发环境](docs/development.md)，本次验证范围见 [M0 验证记录](docs/m0-validation.md)。

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
