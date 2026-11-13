# ADR-0004：POSIX（Linux）输入枚举与安全输出

状态：**提议并实现中（DEV-014 L2 / L3，2026-11-13）**；experimental、未冻结。归档格式不变。

输入：[ADR-0001](0001-experimental-container-and-output.md)（Windows 安全输出）、[ADR-0002](0002-m2-store-archive-constraints.md) §8（事务、复核、取消、信任边界）、[memory-safety](../memory-safety.md)、[POSIX 草案](../posix-filesystem-design.md)、[Linux L1 记录](../linux-l1-validation.md)。

## 1. 范围与原则

- 只针对 **Linux**（glibc，内核 ≥ 5.6 视为目标；能力在运行时探测）。其他 POSIX 系统编译时仍返回 `unsupported_feature`。
- 公共 API（`SafeOutputDirectory` / `SafeOutputFile` / `InputTree` / `InputFile`）与 Windows 相同；调用方（`store_archive.cpp`、seed 研究层、CLI）不分平台。
- 任何一项保证在当前内核 / 文件系统上做不到时**失败关闭**（`unsupported_feature`），不退回可覆盖、可跟随链接或按路径操作的写法。
- **信任边界与 Windows 一致**（ADR-0002 §8.4.4）：同一 uid 与 root 是受信主体（它们可调试本进程或直接改结果）；保证针对的是**其他用户**以及归档内容本身。

## 2. 文件系统与内核能力

| 项 | 决定 |
| --- | --- |
| 接受的文件系统 | 输出目录、输入根所在文件系统按 `fstatfs().f_type` 白名单：**ext4（0xEF53，含 ext2/3 同一魔数）、xfs（0x58465342）、btrfs（0x9123683E）、tmpfs（0x01021994）**。其他（NFS、CIFS/SMB、FUSE、overlayfs、9p/drvfs 即 WSL `/mnt/c`、ecryptfs、zfs 等）一律 `unsupported_feature`。理由：这四种对 `O_TMPFILE`、`RENAME_NOREPLACE`、inode 稳定性、`fsync` 语义有可依赖的本地实现；其余需逐个验证后再加入 |
| 对应 Windows | 仅本地固定 NTFS、拒绝 SUBST / 重定向 |
| `O_TMPFILE` | 必需（文件写出）。`EOPNOTSUPP` / `EISDIR` / `EINVAL` → `unsupported_feature` |
| 发布文件 | `linkat(fd, "", dir, name, AT_EMPTY_PATH)`；内核拒绝（< 6.10 的无特权进程得到 `ENOENT` / `EPERM`）时改用 `linkat(procfd, "<fd>", dir, name, AT_SYMLINK_FOLLOW)`，其中 `procfd` 是 `/proc/self/fd` 目录且 `fstatfs` 确认为 procfs（0x9FA0）。两者都不可用 → `unsupported_feature`。`linkat` 不覆盖（`EEXIST` → `output_conflict`） |
| 发布目录 | `renameat2(..., RENAME_NOREPLACE)`；`EINVAL` / `ENOSYS` → `unsupported_feature`，不退回 `rename` |
| `openat2` | **不需要**：所有打开都是“父目录 fd + 单组件名 + `O_NOFOLLOW`”，单组件不可能穿越目录或经过中间链接，`RESOLVE_BENEATH` / `RESOLVE_NO_SYMLINKS` 不提供额外保证。不依赖它也就不会因旧内核缺它而降级 |

## 3. 逐条等价对照（ADR-0002 §8.4 与 ADR-0001）

| Windows 保证 | POSIX 做法 | 差异 / 剩余风险 |
| --- | --- | --- |
| 根逐级经句柄打开，拒绝重解析点 | 从 `/` 起逐级 `openat(parent, comp, O_PATH \| O_DIRECTORY \| O_NOFOLLOW)`，最后一级 `O_RDONLY \| O_DIRECTORY \| O_NOFOLLOW`；每级 `fstat` 必须是目录。任一组件是符号链接 → 失败 | 与 Windows 一样，输出根路径中的链接一律拒绝（不是解析后再用） |
| 子对象只用父句柄 + 单组件名 | 全部 `*at()` 调用；名字先过归档路径规则且必须恰好一段 | — |
| 同卷 | 记录目标目录 `st_dev`，之后创建 / 打开的每个对象 `st_dev` 必须相同；根路径本身可以跨挂载点（目录树里常见独立的 `/home`） | Windows 从盘符根起就同卷；POSIX 只要求“目标目录及其下”同设备 |
| 固定的父目录不能被改名 / 删除 | POSIX 没有拒绝删除共享。改为**目标目录权限门槛**：`st_uid` 必须是本进程 euid 或 root，且组 / 其他人不可写，或设有粘滞位（如 `/tmp`）。满足时其他用户不能改名、删除或替换我们在其中创建的条目；不满足 → `unsupported_feature` | **比 Windows 更严格**：组可写且无粘滞位的共享目录被拒绝。POSIX ACL 的写权限反映在组权限位（mask）上，同样被拒绝。已打开的 fd 不受父目录改名影响 |
| 暂存目录私有（受保护 DACL，创建时即生效） | `mkdirat(parent, ".seedarc-<随机>.tmp", 0700)`，随后 `openat(O_NOFOLLOW \| O_DIRECTORY)` 并核对：属主为 euid、类型目录、同设备、空目录；`fchmod(0700)` 消除 umask / 默认 ACL 影响。暂存树中的目录 0700、文件 0600，同样 `fchmod` | `mkdirat` 与 `openat` 之间名字可被替换，但只有能改目标目录条目的主体能做到；由上一行的权限门槛排除其他用户 |
| 发布后保留私有 DACL（用户 2026-11-11 已接受） | 发布后的树保留 0700 / 0600，不按 umask 放宽 | 与 Windows 行为一致：共享目录中其他用户不能读取解压结果 |
| 临时文件独占、失败即按句柄删除 | `O_TMPFILE`：文件在发布前**没有名字**，任何人看不到、打不开；失败 / 析构只需关闭 fd，内核回收 | 比 Windows 的随机名临时文件更强；“显式清理”改为 `ftruncate(fd, 0)` 释放空间，关闭时回收 |
| 无替换提交 | 文件 `linkat`（句柄式，`EEXIST` 失败）；目录 `renameat2(RENAME_NOREPLACE)`。改名前后 `fstatat(AT_SYMLINK_NOFOLLOW)` 核对源 / 目标名字指向暂存目录 fd 的 (dev, ino) | 目录改名是按名字的：受信主体在核对与改名之间替换暂存名不在保证范围（同 Windows §8.4.4） |
| 提交前 flush + 整文件读回 | `fsync(fd)`、`fstat` 核对大小、`pread` 全量读回与增量 CRC32C / 前缀逐字节比较；发布后 `fsync` 所在目录 | — |
| 发布前复核：目录恰好包含创建的名字 | `openat(dir, ".")` + `fdopendir` / `readdir` 精确比对字节名（不多不少） | Linux 文件系统区分大小写，“仅大小写不同”的外来名字照样被视为多余条目 |
| 发布前复核：无命名数据流 | 无对应概念。等价检查：对象上不得有 `user.*` 扩展属性（`flistxattr`；`ENOTSUP` 视为无）；`security.*` / `system.*`（SELinux 标签、ACL）由系统维护，允许存在，但权限位必须恰好为 0700 / 0600 | — |
| 发布前复核：文件身份、链接数 1、大小、CRC32C，且排除写者 / 删除者 | `openat(O_RDONLY \| O_NOFOLLOW \| O_NONBLOCK \| O_NOCTTY)` 后 `fstat`：普通文件、(dev, ino) 相同、`st_nlink == 1`、属主 euid、权限 0600、大小相同，然后全文件 CRC32C | **POSIX 不能在打开时排除写者**。暂存树 0700 已排除其他用户；受信主体在复核期间的写入不在保证范围（同 Windows §8.4.4 的“复核后修改”） |
| 失败 / 取消按身份清理，不按路径递归 | 逆序：`fstatat(parent, name, AT_SYMLINK_NOFOLLOW)` 核对类型与 (dev, ino) 后 `unlinkat`（目录 `AT_REMOVEDIR`，只删空目录）；不符的对象保留并报告“清理不完整” | Windows 先开句柄再按句柄删；POSIX 没有按 fd 删除，核对与删除之间只有受信主体能替换名字（暂存树 0700） |
| 取消 | 与 Windows 共用：写循环、提交前、复核中、改名前检查 `stop_token` | — |
| 深度 / 路径长度 / 条目数 | 深度上限 128（自 `/` 起算）同 Windows；路径预算改为 **4095 字节**（`PATH_MAX - 1`，等价路径 `/a/b` 的字节数），使输出仍可被普通按路径的工具访问（Windows 取 32000 UTF-16 单元，也是为此）；条目数、索引、块数等归档限额不变 | 路径预算不同：Linux 上极深的归档路径会以 `resource_limit` 失败 |
| 故障注入 | 复用 `WriteFaults`（短写、写失败 `ENOSPC`、flush 失败、读回损坏、提交失败、显式清理失败、`after_write` 回调），线程局部 | 新增仅测试用的 `force_proc_link`，强制走 `/proc/self/fd` 发布路径，使两条发布路径都被测试 |

## 4. 输入枚举（L2）

| 项 | 决定 |
| --- | --- |
| 根 | 用户给出的路径中间可以经过链接（与 Windows 相同）；**输入对象本身**用 `open(path, O_RDONLY \| O_NOFOLLOW \| O_NONBLOCK \| O_NOCTTY \| O_CLOEXEC)` 打开，是链接则 `ELOOP` → 拒绝；`fstat` 只接受目录或普通文件。根所在文件系统按第 2 节白名单 |
| 子项 | 对目录 fd 用 `fdopendir`（复制的 fd）+ `readdir` 列名；每个名字 `fstatat(AT_SYMLINK_NOFOLLOW)`：符号链接 → `unsupported_feature`（拒绝，不跳过、不跟随）；目录 → `openat(O_RDONLY \| O_DIRECTORY \| O_NOFOLLOW)` 后核对 (dev, ino) 与列举时一致；普通文件只取元数据；**设备、FIFO、socket → `unsupported_feature`** |
| 跨挂载点 | 子项 `st_dev` 与根不同 → `unsupported_feature`（同 Windows“跨卷拒绝”） |
| 硬链接 | 输入中的多链接普通文件按普通文件**各自存一份内容**，不记录链接关系（与 Windows 相同，Windows 也不检查链接数） |
| 读取与变化检测 | 重新逐级 `openat(O_NOFOLLOW)` 打开，核对 (dev, ino)、大小、mtime（纳秒）；用 `pread` 读取；读完再读 1 字节确认 EOF，并再次核对大小与 mtime | **POSIX 不能拒绝其他写者**（Windows 以不共享写打开，使并发写入方直接失败）。并发修改只能事后检测；mtime 精度取决于文件系统，同一时间粒度内、大小不变的修改可能检测不到 |
| 输出归档自身 | 输出是无名 `O_TMPFILE`，不会出现在目录列举中；仍按 (dev, ino) 排除以保持与 Windows 相同的逻辑 | — |
| 限额 | 树深 64、条目、索引、总大小限额与 Windows 共用 | — |

## 5. 文件名编码

- Linux 文件名是字节串。输入名与输出名都必须满足归档路径规则；L1 起非 Windows 只实现该规则的 **ASCII 子集**：畸形 UTF-8 → `unsafe_path`，合法的非 ASCII → `unsupported_feature`。因此**本 ADR 下 Linux 只能压缩 / 解压全 ASCII 名字**。
- 完整支持需要与 Windows `IsNormalizedString(NFC)`、`CompareStringOrdinal(忽略大小写)` 一致的 Unicode 表。候选：从固定版本的 Unicode 数据生成表放入 `third_party/`，并在 Windows 与 Linux 上用同一组差分向量验证。由于无法保证与 Windows 所用 Unicode 版本逐字符一致，这是协议层面的取舍，**另行决定**，本 ADR 不实现。
- 内部的原生名字类型：Windows 为 `std::wstring`，POSIX 为字节 `std::string`（`std::filesystem::path::string_type`）。

## 6. 元数据保留

与 Windows 相同：时间戳、属性、权限、属主、扩展属性都**不保存**到归档。解压结果的权限固定为目录 0700、文件 0600（对应 Windows 的私有 DACL）；mtime 为解压时间。

## 7. 测试要求

- Windows 的 36 项测试在 Linux 上都要有对应项实际运行；DACL 检查换成权限位 + 属主检查；NTFS 命名流换成 `user.*` xattr；junction 换成符号链接；Windows 句柄共享行为（固定目录不能改名、并发写者使压缩失败）在 POSIX 上没有对应，按上表写明并改为检测性断言。
- Linux 专有负面测试：任一路径组件为符号链接（输出根、输入树、暂存树中途被替换为链接）、目标已存在、只读目录、不接受的文件系统、组可写无粘滞位的目标目录、特殊文件输入、清理不误删他人对象、两条 `linkat` 发布路径。
- GCC / Clang Debug / Release 与 ASan + UBSan 全量通过、0 警告；内存有界测试用 `getrusage(RUSAGE_SELF).ru_maxrss`（峰值常驻集，KiB），与 Windows 的峰值工作集口径不同，数值不可直接比较。

## 8. 回滚

改动限于 `core/filesystem/*` 的非 Windows 分支、测试与 CMake。撤销本 ADR 的实现即恢复 L1 状态（Linux 上返回 `unsupported_feature`），不影响 Windows 与归档格式。
