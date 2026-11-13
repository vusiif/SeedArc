# B2-r4 P0 逐块诊断与简单基线（由 b2_diagnose.py 从 raw.json 生成）

共 150 次 compress → verify → extract，全部逐字节还原；同组归档 SHA256 一致。
归档字节 = 头部 96 + 索引 + 数据；Seed / 覆盖元数据未序列化（缺项，非 0）。
全部方案在同一数据集上块边界完全相同（file_key、file_block_index、block_id、raw_bytes 逐块一致）。

## 完整归档字节（越小越好）

| 数据 | store | B1-fixed-zstd3 | zstd6 | lzma6 | b-base | B0-current | b-file | b-cleared | B2-file-codec (codec only) | B2-file-codec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| binary | 393646 | 393646 | 393646 | 393646 | 393646 | 393646 | 393646 | 393646 | 393646 | 393646 |
| compressed | 393834 | 393834 | 393834 | 393834 | 393834 | 393834 | 393834 | 393834 | 393834 | 393834 |
| mixed | 788968 | 394959 | 394959 | 395403 | 461862 | 461862 | 461862 | 461862 | 394959 | 394959 |
| source | 35426 | 9503 | 8835 | 8304 | 9559 | 9560 | 8893 | 9560 | 8300 | 8298 |
| text | 393644 | 944 | 944 | 1379 | 66538 | 66538 | 66538 | 66538 | 944 | 944 |

## 计量对账（B0 / B1 / B2-file-codec）

| 数据 | 方案 | 头部 | 索引 | 数据（逐块和） | 合计 | 对账 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| binary | B0-current | 96 | 334 | 393216 | 393646 | 一致 |
| binary | B1-fixed-zstd3 | 96 | 334 | 393216 | 393646 | 一致 |
| binary | B2-file-codec | 96 | 334 | 393216 | 393646 | 一致 |
| compressed | B0-current | 96 | 381 | 393357 | 393834 | 一致 |
| compressed | B1-fixed-zstd3 | 96 | 381 | 393357 | 393834 | 一致 |
| compressed | B2-file-codec | 96 | 381 | 393357 | 393834 | 一致 |
| mixed | B0-current | 96 | 976 | 460790 | 461862 | 一致 |
| mixed | B1-fixed-zstd3 | 96 | 976 | 393887 | 394959 | 一致 |
| mixed | B2-file-codec | 96 | 976 | 393887 | 394959 | 一致 |
| source | B0-current | 96 | 294 | 9170 | 9560 | 一致 |
| source | B1-fixed-zstd3 | 96 | 294 | 9113 | 9503 | 一致 |
| source | B2-file-codec | 96 | 294 | 7908 | 8298 | 一致 |
| text | B0-current | 96 | 332 | 66110 | 66538 | 一致 |
| text | B1-fixed-zstd3 | 96 | 332 | 516 | 944 | 一致 |
| text | B2-file-codec | 96 | 332 | 516 | 944 | 一致 |

## B0-current 相对 B1-fixed-zstd3 的逐块损失来源

| 数据 | 类别 | 块数 | 字节差（B0 − B1） | block_index |
| --- | --- | ---: | ---: | --- |
| binary | b0_fallback_store | 5 | +0 | 0,1,3,4,5 |
| binary | b0_planned_store | 1 | +0 | 2 |
| binary | **合计** | | **+0** | |
| compressed | b0_fallback_store | 6 | +0 | 0,1,3,4,5,6 |
| compressed | b0_planned_store | 1 | +0 | 2 |
| compressed | **合计** | | **+0** | |
| mixed | b0_planned_store | 5 | +66759 | 2,7,10,12,13 |
| mixed | b0_lzma26_vs_zstd3 | 2 | +144 | 0,4 |
| mixed | b0_zstd4_vs_zstd3 | 3 | +0 | 1,3,5 |
| mixed | b0_fallback_store | 4 | +0 | 6,8,9,11 |
| mixed | **合计** | | **+66903** | |
| source | b0_planned_store | 1 | +56 | 2 |
| source | b0_zstd4_vs_zstd3 | 1 | +1 | 1 |
| source | b0_lzma26_vs_zstd3 | 1 | +0 | 0 |
| source | **合计** | | **+57** | |
| text | b0_planned_store | 1 | +65450 | 2 |
| text | b0_lzma26_vs_zstd3 | 2 | +144 | 0,4 |
| text | b0_zstd4_vs_zstd3 | 3 | +0 | 1,3,5 |
| text | **合计** | | **+65594** | |

## text：逐块对照（B0 / B1 / B2-file-codec）

| block_index | file_key | raw | B0 计划→实际 | B0 stored | B1 实际 | B1 stored | B2 实际 | B2 stored |
| ---: | --- | ---: | --- | ---: | --- | ---: | --- | ---: |
| 0 | text.txt | 65536 | lzma26→lzma26 | 158 | zstd3 | 86 | zstd3 | 86 |
| 1 | text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |
| 2 | text.txt | 65536 | store→store | 65536 | zstd3 | 86 | zstd3 | 86 |
| 3 | text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |
| 4 | text.txt | 65536 | lzma26→lzma26 | 158 | zstd3 | 86 | zstd3 | 86 |
| 5 | text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |

## mixed：逐块对照（B0 / B1 / B2-file-codec）

| block_index | file_key | raw | B0 计划→实际 | B0 stored | B1 实际 | B1 stored | B2 实际 | B2 stored |
| ---: | --- | ---: | --- | ---: | --- | ---: | --- | ---: |
| 0 | 000-text.txt | 65536 | lzma26→lzma26 | 158 | zstd3 | 86 | zstd3 | 86 |
| 1 | 000-text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |
| 2 | 000-text.txt | 65536 | store→store | 65536 | zstd3 | 86 | zstd3 | 86 |
| 3 | 000-text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |
| 4 | 000-text.txt | 65536 | lzma26→lzma26 | 158 | zstd3 | 86 | zstd3 | 86 |
| 5 | 000-text.txt | 65536 | zstd4→zstd4 | 86 | zstd3 | 86 | zstd3 | 86 |
| 6 | sub/binary.bin | 65536 | zstd4→store | 65536 | store | 65536 | store | 65536 |
| 7 | sub/binary.bin | 65536 | store→store | 65536 | store | 65536 | store | 65536 |
| 8 | sub/binary.bin | 65536 | zstd4→store | 65536 | store | 65536 | store | 65536 |
| 9 | sub/binary.bin | 65536 | lzma26→store | 65536 | store | 65536 | store | 65536 |
| 10 | sub/binary.bin | 65536 | store→store | 65536 | store | 65536 | store | 65536 |
| 11 | sub/binary.bin | 65536 | lzma26→store | 65536 | store | 65536 | store | 65536 |
| 12 | sub/packed.gz | 1440 | store→store | 1440 | zstd3 | 131 | zstd3 | 131 |
| 13 | unicode/中文.txt | 24 | store→store | 24 | store | 24 | store | 24 |

## 回退原因汇总（全部方案）

| 数据 | 方案 | 计划 Store | 回退（无空间收益） | 压缩写入 |
| --- | --- | ---: | ---: | ---: |
| binary | store | 6 | 0 | 0 |
| binary | B1-fixed-zstd3 | 0 | 6 | 0 |
| binary | zstd6 | 0 | 6 | 0 |
| binary | lzma6 | 0 | 6 | 0 |
| binary | b-base | 1 | 5 | 0 |
| binary | B0-current | 1 | 5 | 0 |
| binary | b-file | 1 | 5 | 0 |
| binary | b-cleared | 1 | 5 | 0 |
| binary | B2-file-codec (codec only) | 0 | 6 | 0 |
| binary | B2-file-codec | 0 | 6 | 0 |
| compressed | store | 7 | 0 | 0 |
| compressed | B1-fixed-zstd3 | 0 | 7 | 0 |
| compressed | zstd6 | 0 | 7 | 0 |
| compressed | lzma6 | 0 | 7 | 0 |
| compressed | b-base | 1 | 6 | 0 |
| compressed | B0-current | 1 | 6 | 0 |
| compressed | b-file | 1 | 6 | 0 |
| compressed | b-cleared | 1 | 6 | 0 |
| compressed | B2-file-codec (codec only) | 0 | 7 | 0 |
| compressed | B2-file-codec | 0 | 7 | 0 |
| mixed | store | 14 | 0 | 0 |
| mixed | B1-fixed-zstd3 | 0 | 7 | 7 |
| mixed | zstd6 | 0 | 7 | 7 |
| mixed | lzma6 | 0 | 7 | 7 |
| mixed | b-base | 5 | 4 | 5 |
| mixed | B0-current | 5 | 4 | 5 |
| mixed | b-file | 5 | 4 | 5 |
| mixed | b-cleared | 5 | 4 | 5 |
| mixed | B2-file-codec (codec only) | 0 | 7 | 7 |
| mixed | B2-file-codec | 0 | 7 | 7 |
| source | store | 3 | 0 | 0 |
| source | B1-fixed-zstd3 | 0 | 0 | 3 |
| source | zstd6 | 0 | 0 | 3 |
| source | lzma6 | 0 | 0 | 3 |
| source | b-base | 1 | 0 | 2 |
| source | B0-current | 1 | 0 | 2 |
| source | b-file | 1 | 0 | 2 |
| source | b-cleared | 1 | 0 | 2 |
| source | B2-file-codec (codec only) | 0 | 0 | 3 |
| source | B2-file-codec | 0 | 0 | 3 |
| text | store | 6 | 0 | 0 |
| text | B1-fixed-zstd3 | 0 | 0 | 6 |
| text | zstd6 | 0 | 0 | 6 |
| text | lzma6 | 0 | 0 | 6 |
| text | b-base | 1 | 0 | 5 |
| text | B0-current | 1 | 0 | 5 |
| text | b-file | 1 | 0 | 5 |
| text | b-cleared | 1 | 0 | 5 |
| text | B2-file-codec (codec only) | 0 | 0 | 6 |
| text | B2-file-codec | 0 | 0 | 6 |
