# ADR-0009：固实编码的 worker 数按内存预算自动选择

状态：**已接受**（用户 2026-11-27 选择"auto, 50% RAM"）。研究工具已实现 `--jobs auto`；正式 CLI 还没有固实编码，接入固实编码时按本 ADR 设默认值。日期：2026-11-27。起草：主 Agent（Claude）。决定人：用户。

本 ADR 只决定资源政策：worker 数只影响速度和内存，不影响归档字节（DEV-035、DEV-039 实测）。所以它不进入配方、Seed 或归档格式，属于 AGENTS.md 第 3 节的 B 级（默认性能参数）。

## 输入

- **DEV-035**（[记录](../../research/experiments/20261126-parallel-frames-main-001/record.md)）：帧之间相互独立，并行编码的字节与 worker 数无关。印度 4 核机器上，慢配方快 3.3–3.4 倍。
- **DEV-039**（[记录](../../research/experiments/20261127-parallel-memory-main-001/record.md)）：
  - 每个 worker 的内存，在 32 MiB 分帧下是 Zstd-19 111 MiB、LZMA2-9 273 MiB，在 128 MiB 分帧下是 LZMA2-9 799 MiB；
  - 用压缩库给出的估算（`solid_encoder_memory`）算，8 个配方都不低于实测，偏大 1.16–1.59 倍；
  - 8 线程时，慢配方快 4.2–5.2 倍，快速 Zstd 只快 1.2–1.5 倍。

## 决定

1. **默认 `auto`**。正式 CLI 接入固实编码时，默认按下面的规则选 worker 数；用户可以用 `--jobs N` 指定 1–16。
2. **规则**（每一代独立计算）：

   ```text
   预算       = 物理内存 × 50%      （可用 --memory-budget 覆盖）
   可用       = 预算 − 当前常驻内存 − 本代原始字节（编码结果在写出之前的上界）
   每个 worker = 本代最大帧 + solid_encoder_memory(该帧)
   jobs       = clamp(floor(可用 / 每个 worker), 1, min(16, 逻辑核数, 帧数))
   ```

3. **超出预算时**：仍用 1 个 worker 并明确报告（`memory_over_budget`），不悄悄换成更轻的配方，也不改变输出。
4. **快速 Zstd 也适用同一规则**。加速有限，但内存风险也小，不单独设例外。
5. **报告**：每次选择都报告选了几个、受什么限制（内存、核数、帧数、上限 16）以及各项数值。研究工具输出到 stderr。

## 不做

- 不在运行中途根据内存变化调整 worker 数；
- 不把 worker 数写进归档；
- 不改研究工具的默认值（仍为 1），以便和以前实验的计时比较。研究工具需要显式写 `--jobs auto`。

## 风险与限制

- 估算偏大，所以在内存小的机器上可能少开 worker，速度偏保守，但不会超出预算。
- "估算 ≥ 实测"只在 Windows 上用峰值工作集验证过，Linux 和 commit charge 都还没测。正式发布前要补。
- "当前常驻内存"反映的是研究工具整树读入的情况。正式 CLI 流式读入后，这一项会变小，规则本身不用改。
- 物理内存一半是针对单个进程的；同时运行多个压缩任务时，各个任务的预算会叠加。

## 实现（研究工具，2026-11-27）

- `study_common.hpp`：`physical_memory_bytes`、`resident_bytes`（Windows / Linux）。
- `append4_study.cpp`：`choose_jobs`、`--jobs auto [--memory-budget BYTES]`；在 `encode_pending` 里每一代计算一次，并把结果报告到 stderr。
- `core/codec/dictionary.{hpp,cpp}`：`solid_encoder_memory`（DEV-039）。
- 测试 `research.solid_recipe`：
  - `auto` 与固定 worker 数的字节相同；
  - 1 字节预算时报告 `memory_over_budget` 并用 1 个 worker；
  - 预算极大时受帧数或核数限制；
  - 拒绝非法拼写和 0 预算，以及没有 `auto` 时单独给出的 `--memory-budget`。
