# POSIX 输入枚举（L2）与安全输出（L3）设计草案

> 状态：**草案，未实现、未评审、未冻结**（2026-11-13，DEV-014 附带）。L1 中 Linux 上 `InputTree::open` 与 `SafeOutputDirectory` 一律返回 `unsupported_feature`；本文只记录候选方案，供后续 L2 / L3 立项。L3 实现前须先写 ADR，并按 [memory-safety](memory-safety.md) 与 [testing](testing.md) 做完整安全验收。

输入：[ADR-0001](decisions/0001-experimental-container-and-output.md)（Windows 安全输出）、[ADR-0002](decisions/0002-m2-store-archive-constraints.md)（事务语义、输入枚举规则）、[Linux L1 验证记录](linux-l1-validation.md)。

## 0. 目标：与 Windows 实现相同的外部保证

| 保证 | Windows（现行） | POSIX 候选 |
| --- | --- | --- |
| 不跟随链接 | `OBJ_DONT_REPARSE`，拒绝重解析点 | `O_NOFOLLOW` + `openat` 逐段打开；能用时用 `openat2(RESOLVE_BENEATH \| RESOLVE_NO_SYMLINKS \| RESOLVE_NO_MAGICLINKS \| RESOLVE_NO_XDEV)` |
| 子对象只经父句柄、单组件名 | `NtCreateFile` 相对 `RootDirectory` | 目录 fd + `*at()` 系列，组件名不含 `/` |
| 不覆盖既有对象 | `FILE_CREATE`、无替换改名 | `O_CREAT \| O_EXCL`、`mkdirat`（`EEXIST` 即冲突）、`renameat2(RENAME_NOREPLACE)` / `linkat` |
| 不跨卷 | 卷序列号 | `st_dev` 相等（及 `RESOLVE_NO_XDEV`） |
| 对象身份 | 卷 + 128 位文件 ID | `(st_dev, st_ino)`，配合 `st_nlink`、类型、大小核对 |
| 提交前复核 | 前序遍历暂存树、链接数 1、无命名流、CRC | 同样遍历：每个目录恰好包含创建的名字；文件 `st_nlink == 1`、身份不变、大小与 CRC32C 一致 |
| 失败清理 | 句柄删除，核对身份 | `unlinkat` / `unlinkat(AT_REMOVEDIR)` 前先 `fstatat(AT_SYMLINK_NOFOLLOW)` 核对 `(dev, ino)`；不按路径递归删除 |

## 1. 路径 profile（L2 / L3 的共同前提）

- 归档路径规则是**归档契约**，与主机无关：L1 已在非 Windows 上实现 ASCII 子集，非 ASCII 返回 `unsupported_feature`。
- 完整支持需要与 Windows `IsNormalizedString(NormalizationC)` 和 `CompareStringOrdinal(..., TRUE)` 一致的 NFC 判定与简单大写映射。候选：随仓库固定一份 Unicode 版本的表（`third_party/`，记录来源与版本），并用 Windows 与 Linux 跑同一组差分测试向量；**不依赖** glibc locale 或 ICU 的系统版本，否则不同服务器结果可能不同。是否以 Windows 的行为为准、Unicode 版本如何冻结，需 ADR 决定。
- 主机文件系统大小写敏感（ext4 默认）不放宽清单冲突检查：归档仍须能在 Windows 上解开。

## 2. L2：只读输入枚举

1. 根：`open(root, O_PATH | O_NOFOLLOW | O_CLOEXEC)` 后 `fstat`；只接受目录或普通文件；记录 `st_dev`。
2. 目录：`openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)`，`fdopendir` 读取；`d_type` 不可信，每个条目 `fstatat(dirfd, name, AT_SYMLINK_NOFOLLOW)` 后按类型处理。符号链接、设备、FIFO、socket 一律报错拒绝（与 Windows 拒绝重解析点一致），不跳过。
3. 文件：`openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY)`，打开后 `fstat` 核对与枚举时的 `(dev, ino, size, mtime)` 一致；读完后再次核对并确认 EOF。POSIX 没有“不共享写”的打开方式，并发修改只能事后检测（`st_mtim` 精度依赖文件系统，需在 ADR 中写明残余风险）。
4. 名字：目录项字节须是合法 UTF-8 且满足路径 profile；不可表示的名字报错而非改名。排序沿用 ADR-0002 的逐段 UTF-8 字节序，不依赖 `readdir` 顺序。
5. 输出归档排除：输出临时文件先创建，枚举时按 `(dev, ino)` 排除。
6. 限额：深度 ≤ 64、条目 / 元数据限额与 Windows 相同；同时打开的 fd ≤ 深度。

## 3. L3：安全输出（需 ADR + 完整安全验收）

1. **父目录**：从 `/` 起逐段 `openat(..., O_PATH | O_DIRECTORY | O_NOFOLLOW)`（或一次 `openat2(RESOLVE_NO_SYMLINKS)`），持有父目录 fd 直到提交；拒绝跨设备与非目录。
2. **单文件（compress 输出）**：优先 `openat(parent, ".", O_TMPFILE | O_RDWR | O_EXCL, 0600)` 写入、`fsync`、逐字节读回核验，然后 `linkat(fd, "", parent, name, AT_EMPTY_PATH)`（需 `CAP_DAC_READ_SEARCH`，否则用 `/proc/self/fd/N` + `AT_SYMLINK_FOLLOW`）；`linkat` 天然不覆盖（`EEXIST`）。不支持 `O_TMPFILE` 的文件系统回退到随机临时名 `O_CREAT | O_EXCL | O_NOFOLLOW` + `renameat2(RENAME_NOREPLACE)`；两者都不可用时返回 `unsupported_feature`，**不退回可覆盖的 `rename`**。
3. **目录树（extract）**：在父目录下 `mkdirat` 随机名暂存目录（0700），全部对象经暂存目录 fd 与单组件名创建（`mkdirat`、`openat(O_CREAT | O_EXCL | O_NOFOLLOW)`）；写完关闭子 fd、复核整棵树，再 `renameat2(parent, staging, parent, target, RENAME_NOREPLACE)` 提交。
4. **持久性**：文件 `fsync` 后对所在目录 fd `fsync`；提交后对父目录 `fsync`。
5. **权限**：新文件 0600 / 目录 0700 起步，提交前按候选策略 `fchmod`（不保存归档中的权限，ADR-0002 现状）；受 umask 影响的部分须显式处理。
6. **清理**：按创建清单逆序，`fstatat(AT_SYMLINK_NOFOLLOW)` 核对 `(dev, ino)` 后 `unlinkat`；不一致的对象保留并报告“清理不完整”。
7. **故障注入**：沿用 Windows 的线程局部钩子接口（短写、`ENOSPC`、`fsync` / 读回 / 提交失败、取消），保证测试覆盖与 Windows 对等。

## 4. 平台差异与待定问题

- `openat2` 需要 Linux ≥ 5.6、`RENAME_NOREPLACE` 需要文件系统支持（ext4 / xfs / btrfs / tmpfs 支持；部分网络与 FUSE 文件系统不支持）；运行时探测失败即 `unsupported_feature`。glibc 2.39 / 2.43 均提供 `renameat2` 封装；`openat2` 无 glibc 封装，需 `syscall(SYS_openat2, ...)`。
- 竞争窗口：`renameat2` 只保证目标名不被覆盖，攻击者仍可在父目录中预建名字导致失败——这是可接受的 `output_conflict`，不是安全问题。
- WSL 的 `/mnt/c`（9p / drvfs）语义与 ext4 不同（`O_TMPFILE`、`RENAME_NOREPLACE`、inode 稳定性）；安全输出的验收只在原生 Linux 文件系统上做，`/mnt` 路径应明确拒绝或单独测试。
- macOS / BSD 不在当前范围（`renameatx_np(RENAME_EXCL)`、无 `O_TMPFILE`）。
- 测试：symlink / hardlink 预置、目录替换竞争（另一线程反复 `rename` 祖先）、只读目录、`ENOSPC`（小 tmpfs）、跨设备、非 UTF-8 名字、深度与 fd 上限，均需在 ASan / UBSan 下运行。
