# SeedArc Windows 开发环境、构建和依赖

> 状态：**用户提供的环境快照 + 操作规则**，不是程序自动探测结果。快照日期：2026-11-11（整理日期）；执行任务前以实际版本和构建日志为准。

## 0. 当前可执行的 M0 构建入口

现在已有 CMake 工程与六套 Windows presets，第三方依赖尚未引入。以下命令在项目根目录运行；本机实测与限制见 [M0 验证记录](m0-validation.md)，后面的历史版本表仍只是环境快照。

```powershell
cmake --preset msvc-vs2026-debug
cmake --build --preset msvc-vs2026-debug --parallel
ctest --preset msvc-vs2026-debug
```

将 preset 换为 `msvc-vs2026-release` 即可验证 Release。每个 preset 使用独立 build 子目录；VS 的 build / test preset 已指定配置，不依赖生成器默认 Debug。

GCC / Clang presets 要求在当前终端设置 `SEEDARC_UCRT64_ROOT`（以实际安装位置为准），CMake / Ninja 需已在 PATH：

```powershell
$env:SEEDARC_UCRT64_ROOT = 'C:/msys64/ucrt64'
cmake --preset gcc-ucrt64-debug
cmake --build --preset gcc-ucrt64-debug --parallel
ctest --preset gcc-ucrt64-debug
```

也可用 `gcc-ucrt64-release`、`clang-ucrt64-debug`、`clang-ucrt64-release`。presets 仅为各自命令进程优先设置 UCRT64 bin，不修改系统 PATH；直接运行 GNU ABI exe 时仍需在当前终端保证对应 DLL 的搜索路径。机器专用设置可放入不入库的 CMakeUserPresets.json。

通用 CMake 最低版本为 3.28；使用 VS 2026 preset 还需 CMake 支持该生成器，本机实测 4.4.3。没有适用 preset 的平台可用 `cmake -S . -B build/<name>` 并明确工具链，本次未验证 Windows 之外平台。

项目 `.clang-format` 固定源码格式，`.clang-tidy` 启用 Clang 静态分析器检查；不继承用户目录配置。当前未使用 `<print>`，不需要额外链接 stdc++exp。vcpkg manifest 在实际引入压缩依赖时建立。

### CLion MCP 检查

开发时使用 CLion MCP 实际检查相关文件，`get_file_problems` 要求 `errorsOnly=false`，保留 warning 与 error；记录文件范围与是否超时。通过 `get_compiler_info` 核对活动语言标准 / 编译选项，通过 `get_run_configurations` 发现真实运行配置。使用本项目绝对 projectPath；连接信息从已有客户端配置或 IDE 设置取得，不硬编码端口。

**超时的已知原因（CLion 2026.3 / Nova 引擎，2026-11-11 实测）**：

1. C++ 诊断由 ReSharper C++ 后端只对**可见编辑器**运行。对未打开的文件调用 `get_file_problems` 只会建立隐藏编辑器，后端不分析，必然 `timedOut=true`。先 `open_file_in_editor`，再查询。非 C++ 文件（CMakeLists.txt、Markdown）不受影响。
2. CLion 窗口**失去焦点**后（idea.log 出现 `Window focus lost`），即使后端日志已 `UP_TO_DATE 分析已完成`，新切换的编辑器查询仍超时。需要 CLion 处于前台。
3. 外部修改文件后，CLion 需同步并重新分析（数秒）；分析中途的查询可能超时，稍后重查。
4. 判断依据：`%LOCALAPPDATA%\JetBrains\CLion2026.3\log\idea.log`（工具调用、焦点事件）与同目录 `backend.*.log`（`DaemonStateChanged`）。后端 `Cannot resolve the context ... no cached psi file` 告警在正常分析时也出现，不是根因。

`timedOut=true` 且 `errors` 为空时**不代表没有诊断**；记录哪些文件已实际取得结果。

IDE 索引、CMake 或工具链异常可能使诊断不完整，应先区分环境问题与代码问题；不能以空结果宣称全库无警告。不盲目应用全部建议，不擅自关闭检查。构建、CTest 和 Sanitizer 仍需实际执行。

## 1. 开发机器与工具链

- 主机：Windows 10 22H2（19045.6466）x64；16 核；约 31 GB RAM；C 盘在报告时约余 686 GB。
- 主语言：C++23；未来可评估 C++26，Agent 不得因某个编译失败而擅自退回旧标准。

| 类别 | 环境报告状态 | 约束 |
| --- | --- | --- |
| MSVC | `cl.exe` 19.51.36243，VS 2026 Build Tools 18.6.11806；VC Tools 14.51.36231；Win SDK 10.0.26100.0 | 需 VS 生成器或 `vcvars64.bat`；普通 PATH 不包含 `cl.exe` |
| GCC | MSYS2 UCRT64 `g++` 16.2.0 | `C:\msys64\ucrt64\bin`；使用 UCRT64 运行时 |
| Clang | MSYS2 UCRT64 `clang++` 22.1.8，GNU target，libstdc++ | 与 GNU ABI 一致；**不要视作 MSVC / clang-cl** |
| CMake / Ninja / CTest | CMake / CTest / CPack 4.4.3；Ninja 1.13.2 | CMake 生成器含 Visual Studio 18 2026 / Ninja / Ninja Multi-Config |
| GNU Make | MSYS 4.4.1 | 不在 MSVC / Ninja 构建环境中无意调用 MSYS 版 |
| vcpkg | 2026.10.29 已安装，未初始化依赖、未设 `VCPKG_ROOT` | 使用前查实际安装路径、manifest、baseline / triplet / toolchain |
| IDE | CLion 2026.3.0.1、VS Code、Zed | CLion 带 CMake / Ninja / GDB / LLDB / Clang 工具链 |
| Qt | Qt 6.12.0 的 `msvc2022_64`、`mingw_64`；Qt Creator，另有 `C:\Qt\Tools\mingw1310_64` | Qt 是 GUI 后期依赖；不同套件必须做 ABI 和链接验证 |
| 调试 / 分析 | GDB 18.1（UCRT64），clangd / clang-tidy / clang-format 22.1.8，lld 22.1.8 | 独立 LLDB 未装，CLion 内置不等于全局可用 |
| 其他 | Git 2.55.0；Python 默认 3.15、可用 3.11；7z / aria2 / scoop / choco | 不要无授权改系统 Python 或 PATH |
| 报告缺失 | Conan、ccache / sccache / mold、cppcheck、Doxygen、llvm-cov / llvm-profdata、独立 LLDB | **只在当前任务确实需要时上报**；不能假装相关分析已运行 |

三套编译链路曾经实测 C++23 `views`、`<print>`、`std::println`；每次环境或依赖升级后仍须重新验证，尤其是运行时 DLL 与链接。

## 2. 高优先级 Windows 故障排查

### 2.1 Git Bash 的 UCRT64 DLL 遮蔽

Git Bash 常将 `C:\Program Files\Git\mingw64\bin` 提前放入 PATH，其 GCC 运行时 DLL 可能遮蔽 `C:\msys64\ucrt64\bin` 下的 `libstdc++-6.dll`。曾出现 `collect2: ld returned 5 exit status`、空 exe、`Exec format error`、无提示退出码 127 等。

**只修当前 shell 的可靠起点**：

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
command -v g++
g++ --version
```

然后在独立构建目录重新配置、链接并运行 smoke test。优先使用 **MSYS2 UCRT64 shell**，而不是长期依赖混用的 Git Bash PATH。切勿随意将 DLL 复制到各个目录作为“修复”。

### 2.2 MSYS 系统 PATH 污染

报告显示 `C:\msys64\usr\bin` 在系统 PATH 中，可能使 `sh`、`make`、`sed`、`python`、`openssl` 抢占原生工具。使用隔离的开发者终端或项目级工具链配置；**修改系统 PATH 必须先确认**，不得由 Agent 自行修复全局环境。

### 2.3 `<print>`、`stdc++exp` 与 MSVC

在用户提供的 **MSYS2 UCRT64 GNU / Clang GNU ABI** 环境，使用 `std::println` / `<print>` 的目标需链接 `stdc++exp`；MSVC 不需要。如果目标确实使用该功能，可按编译器 / ABI 条件化：

```cmake
if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_link_libraries(seedarc_cli PRIVATE stdc++exp)
endif()
```

`seedarc_cli` 只是建议 target 名称；以真实 CMake target 为准。不要无条件对 Clang 或 MSVC 添加此库。要运行**编译 + 链接 + 执行**测试，不能只看配置成功。

### 2.4 MSVC 与 CMake 4.x

MSVC 下启用 `/Zc:__cplusplus`，避免 `__cplusplus` 报 199711；使用 target 级配置。旧依赖若声明 `cmake_minimum_required(VERSION < 3.5)`，在 CMake 4.x 可能无法配置；优先更新受维护的上游或核对兼容策略，不要擅自降级整个工程。

## 3. 构建配置建议（未必已建工程）

**隔离工具链 / ABI / 构建类型的目录与 CMake cache**；不能在 MSVC、MSYS2 GCC 和 GNU Clang 间复用缓存或二进制对象。推荐后续在有真实 `CMakeLists.txt` 时建立 `CMakePresets.json`，至少覆盖：

- `msvc-vs2026-debug` / `msvc-vs2026-release`：`Visual Studio 18 2026`，x64，多配置；实际生成器存在与否先检测。
- `gcc-ucrt64-debug` / `gcc-ucrt64-release`：Ninja + 明确的 UCRT64 GCC、`CMAKE_BUILD_TYPE`、独立目录。
- `clang-ucrt64-debug` / `clang-ucrt64-release`：Ninja + GNU ABI Clang、独立目录。
- 测试 / Sanitizer 配置只在**已验证支持**的链路启用，不假定所有 Windows ABI / SDK 组合提供同等检测能力。

示例命令（**仅在工程创建、依赖配置正确之后使用**）：

```powershell
# MSVC / VS 2026
cmake -S . -B build/msvc-vs -G "Visual Studio 18 2026" -A x64
cmake --build build/msvc-vs --config Debug --parallel
ctest --test-dir build/msvc-vs -C Debug --output-on-failure

# GCC / UCRT64 —— 先确保 shell 与 PATH 正确
cmake -S . -B build/gcc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build/gcc-debug --parallel
ctest --test-dir build/gcc-debug --output-on-failure

# Clang / UCRT64 GNU ABI —— 必须独立目录
cmake -S . -B build/clang-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe
cmake --build build/clang-debug --parallel
ctest --test-dir build/clang-debug --output-on-failure
```

推荐工程级编译警告基线：MSVC `/W4 /permissive-`，GCC / Clang `-Wall -Wextra -Wpedantic`；具体项目可调整但不以全局禁用作为修复。Release 基准必须使用 Release 且记录编译参数、编码器版本和运行预算。

## 4. vcpkg 与可重复构建

- **默认偏向 manifest mode**：工程中使用 `vcpkg.json` 和明确的 baseline / registry 策略；不要把“本机已经安装过包”当成依赖规范。
- 在实际引入 Zstd / LZMA2 等库时核对 **vcpkg port 名称、CMake target、静态 / 动态链接、triplet、许可证及 Windows / MinGW 支持**；不能假设 MSVC 的二进制可供 GNU ABI 链接。
- 工程 `CMakePresets.json` 应描述跨机器可用的目标，不要硬编码 `C:\Users\...`；机器专用 `VCPKG_ROOT`、MSYS2 安装位置等放到不入库的 `CMakeUserPresets.json` 或经核实的本地环境变量。
- 若缺所需库 / SDK，先提交安装 / 集成方案。包的系统级安装或系统环境修改需授权；仓库内非破坏性配置按 `AGENTS.md` 权限模型执行。
- 不要为了“能编译”而从未知来源复制 DLL、静态库或预编译包。第三方补丁需记录来源、许可证与兼容测试。

## 5. Git 与文档跟踪

最初报告称本地路径 `C:\Users\IceLolly\Music\Clion\SeedArc` 当时不是 Git 仓库；`.gitignore` 曾含 `*.md`。**运行前核实，不把旧状态当作事实**。要移除全局忽略 Markdown 的规则或用明确的 negate 规则确保 `AGENTS.md`、`docs/**/*.md` 被跟踪。

建议合并进项目 `.gitignore`（审核现有内容，不盲目覆盖）：

```gitignore
/build/
/cmake-build-*/
/.idea/
/.vs/
*.obj
*.o
*.pch
*.ilk
*.pdb
*.exe
*.dll
*.lib
*.a
*.exp
*.tmp
CMakeUserPresets.json
```

不要误忽略 `*.md`；若规则在 `.gitignore` 中仍存在，单纯提供新 Markdown 文件不足以让其被版本管理。初始化仓库、修改全局配置和破坏性操作按相应权限确认。

## 6. 环境缺失报告格式

```text
【缺失依赖 / 环境问题】具体项
已确认：运行的检查命令、版本 / 错误、工具链和 shell
为什么必需：关联的当前功能 / 安全 / 性能验收
首选方案：官方 / 可信来源、版本与 Windows 安装 / 配置步骤
如何验证：命令、编译 / 链接 / 运行测试
阻塞范围：不能宣称完成的步骤
可继续范围：不降低标准的其他工作
需用户授权：仅列真正需要外部确认的安装或系统操作
```

如果只是无关的可选工具缺失，写进任务结果的建议即可，不必暂停局部实现。
