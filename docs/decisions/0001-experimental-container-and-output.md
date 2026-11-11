# ADR-0001：M1 实验容器、资源边界与 Windows 输出原型

状态：**提案 / 原型，未冻结，安全 I/O 尚未验收**。日期：2026-11-11。

## 背景与选择

先用零第三方依赖的小型原型验证字节布局、限额与输出安全，再由 M2 接入 Store 全流程。所有实现位于 `seedarc::experimental`；不对外发布归档，不承诺长期兼容，也不改变当前 CLI 的未实现行为。

候选采用固定宽度小端字段和独立 CRC32C。相比直接写 C++ struct，可避免填充 / 本机字节序；相比一次建立完整可扩展格式，先验证最小协议边界更易审查。CRC32C 仅检测意外损坏，不提供认证或加密，后续可对比更强校验的成本。

## 候选头部（96 字节）

| 偏移 | 字节 | 字段 |
| --- | --- | --- |
| 0 | 8 | ASCII SARCEXP + NUL，明确实验身份 |
| 8 / 10 | 各 2 | 实验 revision=1 / header_size=96 |
| 12 / 16 | 各 4 | flags=0 / strategy_version=0 |
| 20 / 22 | 各 2 | seed_length=0 / checksum_id=1（CRC32C） |
| 24 / 32 / 40 | 各 8 | index_bytes / entry_count / block_count |
| 48 / 56 | 各 8 | data_bytes / total_output |
| 64 | 16 | 候选种子空间，当前全零，不解释结构化字段 |
| 80 | 4 | chunk_size_bytes，默认 1 MiB |
| 84 / 88 | 各 4 | 必须为零的保留字段 |
| 92 | 4 | 对前 92 字节的 CRC32C |

CRC32C 反射多项式 0x82f63b78、初值及最终异或均 0xffffffff；空输入结果 0，123456789 结果 0xe3069283。未知版本、flags、策略、算法和校验 ID 显式失败。16 字节种子仅识别候选长度并返回未支持，8 字节等其他长度拒绝；128 位或字段划分未被采纳为稳定契约。

块描述符验证仅接受当前 Store ID=0、配置=0，原始 / 存储长度相同且非零，范围从 0 连续无交叠，累计长度等于头部。块表线格式、文件归属、完整索引、payload 校验覆盖及尾部索引均待 M2 实现；当前 32 字节 entry 最小预算和 48 字节 block 预算只是候选下界，不是已实现索引布局。

## 资源与路径候选

默认索引 16 MiB、条目 100000、块数 1000000、数据区 / 总输出各 8 GiB、单块上限 4 MiB；原型写入一次最多 4 MiB。路径 UTF-8 最多 4096 字节、最多 64 段、每段最多 255 UTF-16 单元。限额先检查再处理，长度运算采用 checked arithmetic。

这些数值是待验证的保守候选，不是实测最优参数。尚未测量总驻留内存；M2 必须将索引数据结构、句柄和缓冲区累计预算纳入验收，不能仅以单字段限额宣称全部资源受控。

Windows profile 要求严格 UTF-8 且已为 NFC，不静默改名；拒绝绝对路径、反斜杠、点段、空段、ADS、设备名、控制字符、尾随点 / 空格及短名歧义字符 ~。清单按主机 ordinal 忽略大小写规则拒绝冲突和文件 / 祖先冲突；实际创建仍必须无覆盖，不能只信任预检查。输入排序候选为规范 UTF-8 路径字节升序；时间戳 / 权限默认不保存、空目录显式条目，具体索引编码与输入变化检测留 M2。

## Windows 安全 I/O 原型与证据边界

候选仅支持本地固定 NTFS 盘的既有绝对根路径：逐级打开非重解析目录，保持祖先句柄；新目录使用 NtCreateFile + FILE_CREATE 原子创建，子对象只使用父句柄相对的单组件名。临时文件从创建起独占打开，写入后 CRC32C、flush、逐字节读回核验，再以 NT 层 `NtSetInformationFile(FileRenameInformation)`、`RootDirectory = NULL`、单组件名与 ReplaceIfExists=FALSE 在文件当前所在（即固定的父）目录内提交；失败按持有句柄标记删除，不退回路径删除。

不使用 kernel32 `SetFileInformationByHandle(FileRenameInfo)`：实测以父句柄作 RootDirectory 返回 error 87，以 NULL 作 RootDirectory 时会把单组件名按进程当前目录解析并把文件移出固定目录。NT 层以父句柄作 RootDirectory 会重开父目录请求写访问，与固定句柄的共享模式冲突（STATUS_SHARING_VIOLATION）。

这是单文件 / 新目录原语，不是完整解压事务。整树回滚、流式大文件、输入枚举 / 变化检测留待 M2。

加固（候选，未冻结）：根盘符须解析为精确的卷 GUID 根（拒绝 SUBST 等目录映射），所有相对对象须与根同卷序列号；累计深度上限 128（自盘符根起算）、等效 DOS 路径 32000 UTF-16 单元，低于 NT 32767 上限以保证普通工具可达；固定目录共享 READ | WRITE、拒绝 DELETE，既不阻塞他人写入 / 移入，也使目录无法被改名或删除。故障注入通过内部线程局部钩子实现，仅测试安装。

**已解除的阻塞**：上一轮的 Windows error 5 来自沙盒账户对用户目录祖先缺少 FILE_TRAVERSE 授权；以实际用户运行即通过，未放宽任何检查。随后修复了提交重命名路径（见上）。junction / hardlink / 已存在目标 / CRC 失败 / 取消前清理等断言已实际执行通过；短写、中途取消、写 / flush / 读回 / 提交失败与显式清理失败已由故障注入测试覆盖。

安全审查子 Agent 提供的是只读设计审查，不是独立运行验证。Microsoft 官方依据：[NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)、[OBJECT_ATTRIBUTES](https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-_object_attributes)、[无替换重命名](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)、[文件共享与硬链接](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createhardlinkw)。这些文档支持方案选型，不替代本机安全测试。

## 当前验证与接续

MSVC / GCC / Clang 的 Debug / Release 及 MSVC ASan 均构建无警告、CTest 18/18；clang-format 与 clang-tidy 通过；头部 / 路径 libFuzzer 各 150 s 短跑无崩溃。CLion IDE 诊断仍超时。命令与限制见 [M1 交接记录](../m1-validation.md)。

回滚可移除新增实验模块与测试目标，不影响 CLI 外部能力或任何正式归档。候选 ADR 不需作为冻结协议审批；正式冻结、持久用户数据使用或放宽安全边界需按项目规则确认。
