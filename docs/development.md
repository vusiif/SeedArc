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

通用 CMake 最低版本为 3.28；使用 VS 2026 preset 还需 CMake 支持该生成器，本机实测 4.4.3。Linux（WSL）presets 与限制见[第 7 节](#7-wsl--linuxl1)。

项目 `.clang-format` 固定源码格式，`.clang-tidy` 启用 Clang 静态分析器检查；不继承用户目录配置。当前未使用 `<print>`，不需要额外链接 stdc++exp。vcpkg manifest 已随 M3 建立（`vcpkg.json` / `vcpkg-configuration.json`，见 [ADR-0003](decisions/0003-m3-zstd-lzma2-codecs.md)）。

### CLion 手工 CMake 配置与依赖

导入 presets 与手工创建的 `Debug` profile 是独立配置。若重新加载命令只有 Qt 的 `CMAKE_PREFIX_PATH`、没有 vcpkg 工具链，`find_package(liblzma CONFIG REQUIRED)` 会失败；增加 Qt 路径不能替代压缩库依赖配置。

手工 MSVC profile 的 CMake options 应包含（将 `<VCPKG_ROOT>` 替换为本机路径；带空格的整个 `-D` 参数须加双引号）：

```text
"-DCMAKE_TOOLCHAIN_FILE=<VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
```

保留已有的其他必要 options。旧构建目录首次切换工具链时使用新目录，或由使用者明确重置该配置缓存；不要把 MSVC 与 MinGW 的缓存或依赖混用。机器路径保存在 CLion 本地 profile，不写入共享 presets。重新加载后核对 CMake 成功、实际编译器与依赖路径，并运行 `seedarc_cli --version`。

### CLion MCP 检查

CLion MCP 按较大开发阶段集中检查：日常小步修改以编译器警告、相关测试和格式检查验证；一个完整研究迭代、功能接入或里程碑验收前，统一检查本阶段相关文件。IDE 独有报错、工具链 / CMake 配置问题或用户指出具体提示时，中途按需检查。纯文档修改无需调用。所有参与 Agent 沿用此节奏，可由协调者在阶段集成后统一检查。服务不可用或超时时记录未覆盖范围与补查安排，继续不受影响的开发，不把未完成检查记为通过。具体诊断时，`get_file_problems` 要求 `errorsOnly=false`，保留 warning 与 error；记录文件范围与是否超时。通过 `get_compiler_info` 核对活动语言标准 / 编译选项，通过 `get_run_configurations` 发现真实运行配置。使用本项目绝对 projectPath；连接信息从已有客户端配置或 IDE 设置取得，不硬编码端口。

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
| vcpkg | VS Build Tools 自带 `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg`，`vcpkg version` 为 2026-03-04（2026-11-11 实查；旧快照记为 2026.10.29）；未设全局 `VCPKG_ROOT` | presets 需在终端设置 `VCPKG_ROOT`；baseline / triplet 见 ADR-0003 |
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
- **当前配置（M3）**：所有 presets 继承隐藏的 `vcpkg-msvc`（`x64-windows-static-md`）或 `vcpkg-mingw`（`x64-mingw-static`），通过 `$env{VCPKG_ROOT}` 使用 vcpkg 工具链；配置前在终端设置 `$env:VCPKG_ROOT`（GNU presets 另需 `SEEDARC_UCRT64_ROOT`）。首次配置会下载并构建 zstd / liblzma，需要网络；已有构建目录改用工具链时须 `cmake --preset <name> --fresh`。MSVC ASan 测试运行时需把 VC Tools 的 `bin\Hostx64\x64` 加入 PATH，否则进程报 0xc0000135。
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

## 7. WSL / Linux（L1）

> 状态：L1（核心库与 codec 编译、不碰文件系统的测试、ASan + UBSan、短时 libFuzzer）已在 WSL 实测，见 [Linux L1 验证记录](linux-l1-validation.md)。**输入枚举（L2）与安全输出（L3）在 Linux 上仍明确返回 `unsupported_feature`**，设计草案见 [POSIX 文件系统草案](posix-filesystem-design.md)。

### 7.1 工作方式

- 用 WSL 自己文件系统里的克隆（例如 `~/SeedArc`），**不在 `/mnt/c/...` 下编辑或构建**；与 Windows 检出只通过 git 同步（push / fetch 或在 Windows 侧 `git fetch \\wsl.localhost\<发行版>\home\<用户>\SeedArc <分支>`）。
- 从 Windows 调用时用 `wsl -d <发行版> -- bash <脚本>`；直接在 PowerShell 里写 `bash -lc '...$x...'` 时变量可能被吞，复杂命令写成脚本文件再运行。
- 缺少系统工具 / 库时报告并由用户安装，Agent 不用 sudo / apt，也不改用 apt 版 zstd / liblzma。
- 长时间压缩测试计划在用户的远端服务器上做（不在 L1 范围）。

### 7.2 环境快照（2026-11-13，WSL `Ubuntu-26.04`，实际检查）

| 项 | 版本 |
| --- | --- |
| GCC / libstdc++ | 15.2.0（`g++`） |
| Clang | 21.1.8（`clang++`，使用 libstdc++；compiler-rt 含 ASan / UBSan / libFuzzer） |
| CMake / Ninja | 4.2.3 / 1.13.2 |
| glibc | 2.43 |
| vcpkg | `~/vcpkg`（microsoft/vcpkg 克隆，工具 2026-10-29），未设全局 `VCPKG_ROOT` |
| 其他 | git、curl、zip / unzip、pkg-config、tar、python3、llvm-symbolizer 已有；**clang-format 未安装**（格式检查改在 Windows UCRT64 clang-format 22.1.8 上做） |

### 7.3 vcpkg 与构建

依赖与 Windows 相同：`vcpkg-configuration.json` 的 git registry baseline + `vcpkg.json` overrides，triplet `x64-linux`（静态库），得到 zstd 1.5.7、liblzma 5.8.4；不使用 apt 的 libzstd / liblzma。

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
cmake --preset gcc-linux-debug
cmake --build --preset gcc-linux-debug --parallel
ctest --preset gcc-linux-debug
```

presets：`gcc-linux-debug` / `gcc-linux-release` / `clang-linux-debug` / `clang-linux-release`、`clang-linux-asan-ubsan`（`-fsanitize=address,undefined -fno-sanitize-recover=undefined`，test preset 设置 `ASAN_OPTIONS` / `UBSAN_OPTIONS`）、`clang-linux-fuzz`（build preset 只构建三个 fuzz 目标，与 `msvc-fuzz` 相同模式）。都继承隐藏的 `vcpkg-linux`（Ninja、`build/<preset>`、仅 Linux 主机可见），Windows presets 未改。编译器名用 `g++` / `clang++`，在其他发行版上若名字不同，用不入库的 `CMakeUserPresets.json` 覆盖 `CMAKE_CXX_COMPILER`。

libFuzzer 短跑（新输入写到临时语料目录，仓库语料只读）：

```bash
cmake --preset clang-linux-fuzz && cmake --build --preset clang-linux-fuzz
mkdir -p /tmp/fz/header && build/clang-linux-fuzz/fuzz/seedarc_fuzz_header /tmp/fz/header fuzz/corpus/header -max_total_time=180
```

### 7.4 Linux 上的功能边界

- 归档路径规则（ADR-0001 的 Windows profile）在非 Windows 上只实现 **ASCII 子集**：规则不变，大小写冲突按 ASCII a–z / A–Z 判定（对 ASCII 与 `CompareStringOrdinal` 忽略大小写等价）；畸形 UTF-8 仍为 `unsafe_path`；**合法的非 ASCII 路径一律 `unsupported_feature`**，因为 NFC 检查和非 ASCII 大小写折叠需要与 Windows 一致的 Unicode 表，L1 不猜测。因此 Linux 只能解析 / 校验全 ASCII 路径的归档。
- `compress` 需要输入枚举、`extract` 需要安全输出，都返回 `unsupported_feature`（CLI 退出码 5，不留任何输出）；`verify` 只读取归档文件，可用。
- 依赖这两者的测试在 CTest 中注册为 DISABLED（显示为 “Not Run (Disabled)”，标签 `windows-only`），原因写在 `tests/CMakeLists.txt` 的清单中。

### 7.5 glibc 注意事项

- WSL 是 glibc 2.43，计划中的远端服务器（Ubuntu 24.04）是 glibc 2.39。在新 glibc 上链接的可执行文件通常不能在旧 glibc 上运行（符号版本 `GLIBC_2.4x`）。
- 用户已说明会自行在服务器上安装所需的 glibc / 工具链；Agent 不处理系统 glibc。较稳妥的做法是在服务器（或同版本容器）上用同一 baseline 重新配置构建，而不是拷贝 WSL 二进制。
- vcpkg 依赖是静态库，但 C++ 运行时与 libc 仍动态链接；跨机器比较压缩输出时记录 glibc、编译器与依赖版本。
