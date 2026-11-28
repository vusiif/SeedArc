# SeedArc 交接快照

更新：2026-11-28。供下一位 Agent 在新对话中接续开发。

**当前进度的唯一入口仍是 [STATUS.md](STATUS.md)。**本文件只是交接快照，不另建任务台账；领取任务、记录结果和阻塞，仍然更新 STATUS。上一版快照（2026-11-24）在 Git 历史里。

## 0. 一句话现状

- 研究工具 `seedarc_append4_study` 的固实配方已经包含以下几项，五棵 dev 树上的**完整归档体积都比 7-Zip 26.03 和 tar|xz -9e 小**：
  - 去重（ADR-0008）；
  - LZMA2 字典字段（ADR-0011）；
  - zstd 压缩的 trailer（ADR-0010）。
- 速度属于中等，资源（内存）偏高。
- 用户的标准是"体积、速度、资源三项中两项强"，所以下一步做速度实验，计划在第 7 节。
- **DEV-044（压缩 trailer）已完成并通过全部验证，但还没有提交**，见第 3 节。

## 1. 接手先读

1. [AGENTS.md](AGENTS.md)、[STATUS.md](STATUS.md)（最新的几条在顶部"当前基线"），以及 `git status --short`。
2. 用户方向：
   - [用户思想汇总](docs/user-vision.md)；
   - [速度 / 体积 / 恢复联合目标](research/ideas/speed-resilience-r1.md)；
   - 本文件第 2 节，汇总了最近几天聊天里的决定。
3. 种子的新设计（只是想法稿，没有实现）：
   - [codec-tree-r0](research/ideas/seed-codec-tree-r0.md)；
   - [policy-layers-r0](research/ideas/seed-policy-layers-r0.md)。
4. 最近的 ADR：
   - [0009 并行内存预算](docs/decisions/0009-solid-encode-worker-budget.md)；
   - [0010 压缩 trailer](docs/decisions/0010-compressed-trailer.md)；
   - [0011 LZMA2 字典](docs/decisions/0011-recipe-lzma2-dictionary.md)。
5. 最近的实验记录，按时间顺序：
   - [DEV-039 并行内存](research/experiments/20261127-parallel-memory-main-001/record.md)
   - [DEV-040 外部基线](research/experiments/20261128-external-baseline-main-001/record.md)
   - [DEV-040b RAR](research/experiments/20261128-external-rar-main-001/record.md)
   - [DEV-041 trailer 测量](research/experiments/20261128-trailer-probe-main-001/record.md)
   - [DEV-042 7-Zip 版本差](research/experiments/20261128-7zip-version-main-001/record.md)
   - [DEV-043 LZMA2 字典](research/experiments/20261128-lzma2-dict-main-001/record.md)
   - [DEV-044 压缩 trailer](research/experiments/20261128-zstd-trailer-main-001/record.md)
6. 做研究之前读 [research/README.md](research/README.md)，按里面的约定归档：run-id 目录、`record.md`、`commands.md`、原始 jsonl。
7. `docs/users_idea/` 里两份原稿的 git blob 哈希，最近一次复核仍是 `0cd267f5…` 和 `0c909c91…`，没有变化。关键设计之前，和交接的时候都要再核对；原稿不许改。

## 2. 用户已明确的决定和方向（2026-11-27 至 11-28，按主题）

**工作方式**
- 只在用户要求时才提交或推送。`docs/debug.log`、`.sandbox-test/`、`.in-tasks/` 永远不提交。
- 提交的内容里不能出现服务器 IP 和本机用户路径。
- 本阶段**暂不做 CLion 检查**。CLion 的 MCP 也连不上。
- 印度服务器（in-long-task skill）是默认的远端：
  - 使用独占模式；
  - 保留 2 GiB 磁盘监控补丁：先应用 `task-monitor-patch.py`，再执行 `git checkout --` 恢复 codec-capabilities 下的 `monitor-patch.json`；
  - 留出集数据只评估一次；
  - 不得修改 `g5_study.cpp`。
- **全局超时规则**（已写进全局 CLAUDE.md 和 `~/.claude/skills/soft-timeout`）：
  - 墙钟超时只是估计：到软超时只提醒，到硬超时才停止。硬超时默认取 max(3 × 软超时, 软超时 + 30 min)。
  - 被截停时要报成"被硬超时截停"，不能报成失败。
  - 安全阀不放宽：fuzz 单条输入的超时、内存和磁盘上限、远端任务预算。
  - 注意：CTest 的 `--timeout` 盖不过测试自己的 TIMEOUT 属性。

**评价标准**
- 体积、速度、资源三项里，**有两项强就够了**。
- "较优"由用户的偏好决定：要快，包可以大一点；要小，时间可以长一点。
- 策略种子的验收线是**不输给同一偏好档的强默认**。比的是完整归档的字节数；时间要如实记录，但不设门槛。
- 偏好档的具体划分和各档的强默认，等速度数据出来再定。

**种子设计**（想法稿，没有实现；改变 Seed 表示属于 C 级）
- 种子定长，但内部各段的长度不固定：先是公共前缀，再是前缀码标签（`0` Zstd、`10` LZMA2、`110` Store、`111` 保留），然后是算法专用的参数段。
- 每一代各有一个小种子，存完整配方。
- 大种子由工具从整个包算出来，追加以后会跟着变。
  - 定长的大种子只能用来认出和检验一个包，没法复现它，原因是抽屉原理。
  - 要完整复现，用变长的历史种子：全局段 + 小种子链 + 经过熵编码的每个文件的代号。它的前提是追加只增不改，这一点还没核对。
- 文本形式：配方部分保持可读；代号部分用 Base64url，末尾带校验位；太长时改存成 `.seed` 文件。
- 三层分工：
  - 定长的**策略种子**记录压缩思想，比如 128 位，由分类器和每一类的配方槽组成；
  - **计划**写进包里；
  - 不定长的 **`.seed` 文件**记录每个文件的强哈希和帧布局，供没有旧包时做增量重压。增量重压还要解决帧边界连锁移动的问题，属于 C 级。
- **彩蛋种子**：带自然语言字面意思的字符串，完全独立于策略种子系统。
  - 它可以是官方建议，也可以是好玩的压法，都必须向用户说清楚，比如提供 `egg --list`。
  - 建议类彩蛋直接展开成一个官方策略种子，用户已同意复用这套机制。
  - 六条约束用户已经全部接受：用注册表、不做哈希、加 `egg/` 前缀、解码不依赖彩蛋、行为确定且资源有界、"最优"只在自己的空间里成立、先做完主体再做彩蛋。
  - 彩蛋叫什么名字，由用户来定。

**外部对比**
- DEV-040 允许上传全部 dev 树、做大流量传输、按需安装工具。这个授权**只适用于那一次实验**。
- 外部对比的目的是吸收别人的长处：差距要拆到具体来源，不能只报谁赢谁输。
- RAR 只能在本机用用户的许可证测；结果能否公开，要等用户看过许可条款。

## 3. 仓库与工作区状态

- 分支 `main`，HEAD 是 `cab6a68`（DEV-039 到 DEV-043），已经推送到 origin。
- **还没提交的是 DEV-044 和超时调整**：
  - research/tools/ 下的 append4_study.cpp、recipe_commands.hpp、bounded_extract.hpp、append4_fuzz.cpp、check_solid_recipe.cmake、check_solid_search.cmake、CMakeLists.txt（solid_recipe 测试的 TIMEOUT 从 900 改为 2400，注释写了原因）；
  - 两个 fuzz 样本 `r5-trailer-zstd-{seed,explicit-append}.sadx`；
  - docs/decisions/0010 改为"已接受"，并加了实现记录；
  - research/experiments/20261128-zstd-trailer-main-001/；
  - STATUS.md；
  - 本文件。
- 这些改动的验证都做过了：MSVC 50/50、WSL GCC 51/51、ASan+UBSan 22/22、fuzz 1 201 s 无崩溃样本。**提交和推送都要等用户开口。**
- 永远不提交的有 `.in-tasks/`（印度任务回执）、`.sandbox-test/`、`docs/debug.log`。

## 4. 已实现的能力（研究工具，不是正式格式）

### 4.1 配方：映射修订 3，ADR-0006 之后又扩充了四次，已有值的含义都没变

| 位（Seed 形式） | 字段 | 说明 |
| --- | --- | --- |
| 0–4 | 分组 rule（2 位）和 k−1（3 位） | k 取 1–8 |
| 5–7 | 帧档 | 0 表示整体（上限 1 GiB），其余是 16、32、64、128、256 MiB |
| 8–9 | 算法 | 0 Zstd（级别 1–19），1 LZMA2（预设 0–9） |
| 10–14 | 级别 | |
| 15 | extreme（ADR-0007） | 只用于 LZMA2 |
| 16 | dedup（ADR-0008） | 同一代、同一组内，内容完全相同的文件做成别名 |
| 17–19 | dict 码（ADR-0011） | 只用于 LZMA2：0 表示预设，1–4 依次是 128、256、512、1024 MiB，5–7 保留；只影响编码，解码不变 |
| 20–31 | 保留 | 必须为 0 |

- **显式形式**：第二个字的 bit 16 是 extreme，17 是 dedup，18–20 是 dict，21 及以上保留。
- **文本形式**：`solid/<codec>-<lvl>[e][-dict<N>MiB]/g<k>-r<rule>/frame-<whole|NMiB>[/dedup]`，必须是规范拼写。
- 目前最好的配方：`solid/lzma2-9e-dict256MiB/g1-r0/frame-whole/dedup`。

### 4.2 容器：修订 5

- **头部标志**：bit 0 是 seed，bit 1 是 solid，bit 2 是 zstd trailer（ADR-0010，只有修订 5 能用）。其他位都会被拒绝。
- **trailer 的存储形式**：raw，或者 `原始长度(8) | 一个 zstd-19 帧`。
  - 原始长度的上限是 256 MiB（fuzz 构建里是 16 MiB），先检查长度再分配内存。
  - zstd 帧必须恰好一个、内容大小和原始长度相等、不带字典、窗口不超过 2^27、帧后面不能有多余字节。
- **形式的规则**：
  - 新包默认用 zstd trailer；
  - 追加时沿用原包的形式；
  - 合并时默认沿用原包的形式，`--trailer` 可以改。

### 4.3 研究工具常用命令

```text
seedarc_append4_study [--jobs N|auto [--memory-budget B]] [--trailer raw|zstd] COMMAND ...
  recipe-pack seed|explicit RECIPE DIR OUT      recipe-append OLD NEW_DIR OUT [RECIPE]
  consolidate[-check] ARCHIVE [OUT] [RECIPE]    describe ARCHIVE   verify ARCHIVE DIR
  extract ARCHIVE PATH OUT_DIR NAME（只读索引）  memory-estimate RECIPE
  trailer-recode ARCHIVE OUT raw|zstd（单代）   trailer-fault ARCHIVE OUT CASE（测试用）
  codec-probe / dup-probe / cut-probe（只测量，不改格式）
```

- `--jobs auto` 的规则（ADR-0009）：
  - `jobs = clamp(⌊(预算 − 常驻 − 本代原始字节) / (最大帧 + solid_encoder_memory)⌋, 1, min(16, 核数, 帧数))`；
  - 预算是物理内存的 50%；
  - 选择结果以 `{"jobs_auto":…}` 输出到 stderr。
- **已知限制**：
  - 研究工具会把整棵树、整个包一次读进内存；
  - 整体帧只有一帧，所以不能并行；
  - 同一路径的修改、删除、重命名都不支持。

### 4.4 研究脚本（research/scripts/）

- `external_baseline.py`、`_task.py`、`_package.py`：外部基线对比，用在印度。
- `external_rar.py`：本机测 RAR。
- `trailer_probe.py`：测量 trailer。
- 每次实验用到的仓库外临时脚本，内容都抄在对应实验目录的 `commands.md` 里。

## 5. 关键结果（dev 数据，未碰留出集）

**完整归档字节数**：第一列是 SeedArc 的最终配方（256 MiB 字典、整体帧、去重、zstd trailer）。

| 树 | SeedArc | 7-Zip 26.03 -mx9 | tar\|xz -9e | 7-Zip 23.01 | RAR 7.23 -m5 -s -md1g |
| --- | ---: | ---: | ---: | ---: | ---: |
| append-r1 dev | 35 378 301 | 36 065 458 | 35 445 856 | 36 080 089 | 40 203 635 |
| cargo | 18 549 109 | 18 931 714 | 19 539 944 | 19 789 676 | 22 501 935 |
| rustup | 44 287 436 | 46 553 189 | 48 782 200 | 50 486 046 | 50 280 541 |
| 全部 | 64 378 752 | 67 016 273 | 69 937 464 | 71 903 675 | 74 773 550 |
| npm | 1 653 715 | — | 1 659 804 | 1 661 721 | — |

**差距来源**（DEV-040、042、043、044）
- trailer 原来不压缩，每个文件占 71–109 B；zstd 压缩后约为原来的 1/4–1/5。
- 7-Zip 26 比 23 小，**全部来自 `-mx9` 默认字典从 64 MiB 改成 256 MiB**。同一 liblzma 下，xz 用 256 MiB 字典的结果和我们只差几百字节。
- BCJ2 过滤器在 rustup 上反而让结果大 4.7%；按类型排序也没有收益。
- `zstd --long` 在 cargo 上小 16.5%，所以 Zstd 槽的窗口和 LDM 值得测。
- 去重是我们独有的收益。

**速度**（印度，4 vCPU，单线程固定在 CPU 0，用的是 64 MiB 字典时代的配方）
- 最小体积的配置是单线程，"全部"树上 SeedArc 348 s，xz 380 s，7z 298 s。
- 多线程时 SeedArc 的 `--jobs auto` 最快（90 s，7z 106 s），但只有 32 MiB 帧能并行，体积要大约 6%。
- 本机加上 256 MiB 字典后，单线程大约是 7-Zip 26 的 0.9–1.8 倍。

**资源**
- 打包的峰值内存：64 MiB 字典时 1.3–2.1 GB，256 MiB 字典时 2.8–4.2 GB。对比：xz -9e 是 0.7 GB，7z 26 是 2.0–2.7 GB。
- 解码的内存不受字典大小影响。

**单文件提取**：32 MiB 帧取一个文件 0.04–0.22 s，和 7z 的小块模式同档；整体帧和 xz 差不多。

**限制**：
- 每个格子只跑了一次；只用了 dev 数据；字典大小是在这几棵树上选的。
- 外部工具没有调到极限，比如 7z 的 `-md=1g`、zstd 的 `--ultra -22 --long=31`。
- RAR 能不能公开，还没确认。

## 6. 构建、测试与环境

- **本机 MSVC**，研究工具开启的构建目录是 `build/dictstudy-check`（VS 2026 生成器，Release 配置）：
  ```text
  cmake --build build/dictstudy-check --config Release --parallel
  ctest --test-dir build/dictstudy-check -C Release -j 4        （50 项；solid_recipe 约 6–7 min）
  ```
  注意：`build/msvc-vs2026-release` **没有打开研究工具**，不要用它。
- **WSL 构建**（GCC 14 Release 和 Clang ASan+UBSan）：在 WSL 原生克隆 `~/SeedArc-append` 里进行，通过 git 同步。
  1. 用临时的 `GIT_INDEX_FILE` 做 `read-tree HEAD`，再 `add -A -- core research tests fuzz cli docs STATUS.md CMakeLists.txt ':!research/local' ':!docs/debug.log'`；
  2. `commit-tree`，把结果写到 `refs/wip/drift`；
  3. 在 WSL 里 `git fetch` 这个 ref，然后 `checkout --detach`；
  4. 依次构建 `build/ap-gcc-linux-release` 和 `build/ap-clang-linux-asan-ubsan`；跑 GCC 的全部 CTest，以及 ASan 下与研究相关的 22 项；
  5. 跑完删掉这个 ref，本机和 WSL 两边都要删。

  libFuzzer 构建目录是 `build/ap-fuzz`，目标是 `seedarc_fuzz_append4`，常用参数 `-max_total_time=1200 -rss_limit_mb=800 -malloc_limit_mb=600 -max_len=16384 -timeout=30`。

  上一个会话的脚本放在会话临时目录里，可能已经被清掉。按上面的步骤自己重写即可。
- **跨工具链核对**：固定的小包，GCC 和 MSVC 打出来的 SHA 必须相同。三个例子：两个 dict fuzz 样本 `59120c7b…` / `daf28be6…`，`far` 树的 `364a994a…`。
- **印度**：用 `in-long-task` skill 的 `in_task.py`（preflight / prepare / start / status / fetch / clean）。
  - 输入目录上限 16 MiB，大数据要另外用 scp 传，两端都要核对 SHA。
  - 固定的工具链和 vcpkg（GCC 14.2、zstd 1.5.7、liblzma 5.8.4）借用 `/home/sc/long-tasks/20261123T123105Z-d5596074/work`。**这个目录不能清理。**
  - DEV-040 的三个任务都已经 fetch 并 clean。
  - 服务器上还留着 `/home/sc/seedarc-data/dev040`（131 MB 的 dev 树打包，SHA `145b30a6…`）。**删不删由用户决定。**
- **本机 dev 树**都在 `research/local/` 下，这个目录不入库：
  - `append-r1/files/dev`；
  - `dup-local-dev-r1/files/{cargo,rustup,npm}`。

  它们和 DEV-040 的上传清单逐个文件对得上：20 001 个文件，清单 SHA `a9dbb0a5…`。
- 工具版本：本机 Python 3.15.2，自带 `compression.zstd`；印度是 Python 3.12，**没有** `compression.zstd`。所以测试夹具不能依赖 Python 做 zstd 压缩。

## 7. 下一步计划（建议顺序）

1. **问用户是否提交 / 推送 DEV-044**，相关改动见第 3 节。
2. **速度实验**：这是主线的下一项，我已经向用户提出，**用户还没点头**，开始前要先确认。
   - 在印度测。帧大小取 64 / 128 / 256 MiB 和整体，字典取 128 / 256 MiB，都开 `--jobs auto`。
   - 对照组用 7z 的默认线程数和 xz -T0。
   - 纯解码时间单独测，现在的 verify 里还带着和源树的比对。
   - 要看的是：体积损失多少，换来多少提速。参照点：7z 多线程只比单线程大 0.03%。
3. **定偏好档**：根据第 2 步的数据，定下快档和小档各自的强默认与预算，写一份 ADR。属于 C 级。
4. **流式读入**（体积加资源）：输入改成流式，配有界的帧缓冲，把内存降到大约"字典 + 一两帧"。
5. **留出集一次性评估**：先写好方案，再只跑一次。
6. 种子的新表示：codec-tree / policy-layers / 彩蛋这几项，等偏好档定了以后再写 ADR，其中种子长度 W 用 32 还是 128 位要先定。
7. 其他待定事项：
   - Zstd 槽的 windowLog / LDM 字段；
   - 128 MiB 和 512 MiB 两档字典还没在 dev 树上测；
   - 跨代去重；
   - consolidate 的触发阈值；
   - 研究配方正式接入 CLI；
   - CLion 的阶段检查，等 IDE 可用时再做。

## 8. 坑与经验

- **Bash heredoc**：内容里只要有某些字符组合，带引号的 heredoc 也会被搞坏，比如出现 `unexpected EOF`、反斜杠丢失。改动大的 Python 补丁，先用 Write 写成临时文件再执行。
- **Git Bash 调 `wsl.exe` 传路径**时，要加 `MSYS_NO_PATHCONV=1`，否则 `/mnt/c/...` 会被改写。
- **用 Python 的 `importlib` 加载仓库里的脚本，会生成 `research/scripts/__pycache__`**。运行时加 `-B`；做快照或提交之前，先确认没有 `__pycache__`。
- **subprocess 用相对路径找不到 exe**（WinError 2），要传绝对路径。
- **7z 会把以 `@` 开头的参数当成列表文件**，所以要用 UTF-8 列表文件加 `-spd -scsUTF-8`。RAR 的列表文件要用 Windows 分隔符和 `-scfl`。
- **recipe_fixture.py 的 retag / realias 会直接改 raw trailer**，所以给它们的源包必须用 `--trailer raw` 打包。
- **测试里比较 Seed 和显式两种表示的大小时，要用 raw trailer**：两种表示的 GEN 记录内容不同，zstd 压缩后大小可能差一两个字节。
- **Sanitizer 构建比 Release 慢 2–4 倍**，而且和 fuzz 同时跑会更慢。solid_recipe 单独跑大约 800 s，TIMEOUT 已设为 2400 s。
- **`ZSTD_estimateCCtxSize_usingCCtxParams` 在 zstd 1.5.7 开启 LDM 时会除零。**所以 `solid_encoder_memory` 改成真正建一个 CCtx，再读 `ZSTD_sizeof_CCtx`。
- 印度 apt 下载 7zip 时会选中 ESM 版本，导致 401。用 `apt-cache madison` 选非 ESM 的版本。
- 印度的 vcpkg xz 要加 `-DBUILD_TOOLS=ON` 才有 xz 命令行工具。

## 9. 本次交接时的核对

本文件写于 DEV-044 验证完成之后。我只修改了本文件和 STATUS 顶部的链接，没有新的构建、测试或远端操作，也没有提交。
