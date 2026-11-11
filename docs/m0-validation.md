# M0 工程骨架验证记录

日期：2026-11-11。验证者：本次主 Agent 直接执行，无外部执行者报告。代码当前未提交，不存在可引用 commit；记录对应本次工作区，不构成正式发布版本。

## 实现范围

- CMake 工程、六套独立 Windows configure / build / test presets。
- C++23 静态核心库 libseedarc、别名 SeedArc::core、CLI 目标 seedarc_cli（输出 seedarc）。核心无 Qt 或第三方依赖。
- 版本 0.1.0 由 CMake PROJECT_VERSION 生成；这是工程版本，不是归档格式版本。
- Error / ErrorCode 与 `std::expected<uint64_t, Error>` 加法 / 乘法溢出检查，先核验再运算。
- CLI 帮助、版本；错误参数退出 2，未实现命令退出 3。尚无归档读写或压缩能力。
- 13 项 CTest：核心边界检查，以及 help / short_help / help_command / version / missing / unknown / extra / compress / extract / verify / search / benchmark。核心测试不用 assert，Release 同样检查失败。

## 实测工具链与结果

CMake / CTest 4.4.3，Ninja 1.13.2，Windows SDK 10.0.26100.0；MSVC 19.51.36243.0（VC Tools 14.51.36231），GCC 16.2.0，Clang 22.1.8（MSYS2 UCRT64 / GNU ABI）。

| 配置 | 构建 / 链接 | CTest |
| --- | --- | --- |
| msvc-vs2026-debug | 通过 | 13/13 |
| msvc-vs2026-release | 通过 | 13/13 |
| gcc-ucrt64-debug | 通过 | 13/13 |
| gcc-ucrt64-release | 通过 | 13/13 |
| clang-ucrt64-debug | 通过 | 13/13 |
| clang-ucrt64-release | 通过 | 13/13 |

每套执行：`cmake --preset <name>`、`cmake --build --preset <name> --parallel`、`ctest --preset <name>`。GNU presets 使用当前进程 `SEEDARC_UCRT64_ROOT=C:/msys64/ucrt64`。原始 CTest 日志在 `build/<name>/Testing/Temporary/LastTest.log`；构建目录不入库，表中通过数不属于压缩实验或性能数据。

## 格式与静态检查

对 core / cli / tests 下全部 .cpp / .hpp 执行 `clang-format --dry-run --Werror`，通过。Clang 静态分析器对四个 .cpp 执行以下命令，退出 0，无诊断：

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
clang-tidy --config-file=.clang-tidy -p build/clang-ucrt64-debug core/checked_arithmetic.cpp core/version.cpp cli/main.cpp tests/core_tests.cpp
```

首次不指定配置时 clang-tidy 尝试读取用户目录配置失败；新增项目 .clang-tidy 并显式指定后通过，没有关闭失败检查。

## AddressSanitizer

使用独立目录执行 MSVC Release ASan，不复用普通配置缓存：

```powershell
cmake -S . -B build/msvc-asan -G 'Visual Studio 18 2026' -A x64 '-DCMAKE_CXX_FLAGS=/EHsc /fsanitize=address /Zi' '-DCMAKE_EXE_LINKER_FLAGS=/DEBUG /INCREMENTAL:NO'
cmake --build build/msvc-asan --config Release --parallel
$env:PATH = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64;' + $env:PATH
ctest --test-dir build/msvc-asan -C Release --output-on-failure
```

ASan DLL 路径是本次实测安装位置，复现时按实际 VC Tools 版本修改；不向项目或系统目录复制 DLL。首次覆盖 CMAKE_CXX_FLAGS 时遗漏 /EHsc，且链接未加 /DEBUG，出现 C4530 / LNK4302；已补全正常异常语义和调试信息后重新验证。

最终结果：构建成功，无上述警告，CTest 13/13 通过，无 ASan 报告。日志位于 `build/msvc-asan/Testing/Temporary/LastTest.log`。

## 限制与下一步

这仅验证现有骨架、边界函数和 CLI 行为，不证明归档解析 / 解压安全、跨平台兼容或种子复现。未运行 Fuzz、压缩往返、基准或研究实验，相关功能尚不存在；研究目录没有虚构结果。

下一步 M1：实验格式、资源限额、Windows 安全 I/O 候选 ADR 与原型测试，不直接冻结格式。
