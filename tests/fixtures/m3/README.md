# M3 实验归档固定样例

来源：已验收提交 `77764b9` 的 MSVC Release CLI，2026-11-12 主 Agent 生成。二进制 SHA256：`3a3b0fcc414d25288b8763d76233931fca5d8f709407b485d9a254ff1f4b102c`。归档 revision 1，strategy_version 0，无 Seed。文件大小与哈希见 manifest.json。

- zstd-text.sarc / lzma2-text.sarc：各一个 text.txt，262144 字节。内容由 `(b"SeedArc synthetic baseline: fixed chunks and explicit codec choices.\n" * 4096)[:262144]` 生成。默认 1 MiB 分块，等级分别 3 / 6；完整生成命令、输入哈希与执行证据见 [基线记录](../../../research/experiments/20261112-m3-baseline-main-001/record.md)。
- store-empty.sarc：空输入目录，`seedarc compress <empty-directory> -o <archive> --codec store`；0 条目 / 0 块 / 0 输出字节，归档 96 字节。

均为项目自生成数据，GPL-3.0-or-later。用于解码回归，不构成长期格式冻结；修改时须保留原样例或明确记录兼容决定，不能在普通重建中再生成覆盖。测试核对完整解析、受限解码、校验与摘要，并拒绝截断；原实验另外验证文件清单和内容哈希往返一致。
