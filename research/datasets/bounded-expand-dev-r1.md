# bounded-expand-dev-r1

2026-11-25, DEV-023. Two new trees built from existing public dev data only; **not a holdout**. Rules are in [bounded_expand_manifest.py](../scripts/bounded_expand_manifest.py) and were fixed and applied before any DEV-023 compression command ran. Machine data: [metadata.json](bounded-expand-dev-r1/metadata.json).

| Class | Source | Files | Bytes | Largest file | Manifest SHA256 |
| --- | --- | ---: | ---: | ---: | --- |
| source | Linux 6.12 files from [append-linux-large-dev-r2](append-linux-large-dev-r2.md) 512 MiB dev, extensions `.c .h .S .rs .asm .py .sh .pl` | 11,285 | 268,433,674 | 16,414,003 | `c6e0d158…f3e15` |
| text | enwik8 dev slices + Silesia dev files that pass the text rule | 49 | 81,553,445 | 33,553,445 | `f539c811…57021` |

## Rules

- **Source:** the 21,008 qualifying Linux files, ordered by `sha256("dev023-select:" + path)`, longest prefix ≤ 256 MiB; stop at the first file that does not fit. No truncation or padding.
- **Text:** a file counts as text if it contains no NUL, has at most 4 invalid UTF-8 sequences (allowance for the 1 MB slice boundaries), and C0 controls / DEL are ≤ 0.1% of its bytes. All qualifying files are used.
- **Per-file limit:** 32 MiB, by size only. `silesia/nci` (33,553,445 B) is just under it.
- **Excluded by the text rule** (recorded in metadata): `enwik8/part-004.xml` (6 invalid sequences), `enwik8/part-033.xml` (8); `silesia/reymont`, `samba`, `sao`, `xml` (contain NUL; `xml` is a tar file). The rule was not changed after seeing these.
- **Added batches:** order the final tree by `sha256("dev023-batch:" + path)` and take the prefix whose bytes are closest to the target share. The parent is the rest.

| Class | added10 files / bytes / share | added40 files / bytes / share |
| --- | --- | --- |
| source | 553 / 26,841,670 / 9.999% | 3,723 / 107,364,961 / 39.997% |
| text | 8 / 8,000,000 / 9.810% | 33 / 33,000,000 / 40.464% |

The two histories of one class share the same final tree, so their fresh rebuilds must be byte-identical.

## Limits

- Text is 77.8 MiB, below the 128 MiB target. Per the plan all qualifying files are used and nothing is padded. The 128 MiB frame limit and the 1 GiB reference can therefore give the same layout.
- enwik8 slices are the original append-r1 1 MB cuts, not natural files.
- Both production manifest checks passed (`seedarc_path_check --manifest`: source 13,669 entries including parent directories, text 51).
- Contents stay in ignored `research/local` / remote task directories. Licences follow the source datasets: Linux GPL-2.0, enwik8 CC BY-SA, Silesia per file.
