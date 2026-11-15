# SPDX-License-Identifier: GPL-3.0-or-later
"""Build append-r1 (DEV-017, research/ideas/seed-append-r1.md section 7.1).

Four pools (linux-src, native-bin, silesia, wiki-text), each split by whole groups into dev and
holdout with disjoint content, and each split ordered into an initial set (first 70 % of bytes)
plus three append batches of about 10 % each. Every path passes the production path profile
(seedarc_path_check, ADR-0005) and each split's full list passes the production manifest check
(--manifest: duplicates, case collisions, file-as-directory), after a stricter casefold()
pre-filter that drops every member of a possibly colliding set. Data goes to the ignored
research/local/append-r1/files/<split>/<pool>/, listings to research/datasets/append-r1/.

  python -I research/scripts/append_corpus.py --path-check <seedarc_path_check>
  python -I research/scripts/append_corpus.py --check
"""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
CORPUS = "append-r1"
LOCAL = ROOT / "research/local" / CORPUS
META = ROOT / "research/datasets" / CORPUS
MiB = 1024 * 1024
WSL = Path(r"\\wsl.localhost\Ubuntu-26.04")
LINUX_ROOT = WSL / "home" / os.environ.get("USERNAME", "").lower() / "corpora/linux/linux-6.12"
LINUX_LISTING = LOCAL / "listings/linux-6.12-extracted.tsv"
SILESIA = LOCAL / "raw/silesia"
ENWIK8 = LOCAL / "raw/enwik8/enwik8"
NATIVE = [
    ("msvc-lib-x64", Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\lib\x64"), (".lib",)),
    ("ucrt64-bin", Path(r"C:\msys64\ucrt64\bin"), (".dll", ".exe")),
    ("ucrt64-lib", Path(r"C:\msys64\ucrt64\lib"), (".a",)),
]
SPLIT_CAPS = {"linux-src": 48 * MiB, "native-bin": 64 * MiB}
NATIVE_SIZE = (64 * 1024, 32 * MiB)
ENWIK8_PARTS, ENWIK8_PART_BYTES = 100, 1_000_000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def key_hash(text):
    return hashlib.sha256(f"{CORPUS}:{text}".encode()).hexdigest()


def native_group(name):
    m = re.match(r"[A-Za-z]+", name)
    return (m.group(0) if m else name).lower()


def candidates():
    """pool -> list of dict(rel, group, size, sha?, read())."""
    pools = defaultdict(list)
    # linux-src: from the extraction listing (path, size, sha256), read only when selected
    for line in LINUX_LISTING.read_text(encoding="utf-8").splitlines():
        rel, size, digest = line.split("\t")
        rel = rel.removeprefix("linux-6.12/")
        parts = rel.split("/")
        group = "/".join(parts[:2]) if len(parts) > 2 else parts[0]
        pools["linux-src"].append({"rel": f"linux/{rel}", "group": f"linux/{group}", "size": int(size),
                                   "sha": digest, "src": LINUX_ROOT / rel})
    for label, root, exts in NATIVE:
        for p in sorted(root.iterdir()):
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in exts:
                n = p.stat().st_size
                if NATIVE_SIZE[0] <= n <= NATIVE_SIZE[1]:
                    pools["native-bin"].append({"rel": f"{label}/{p.name}", "group": f"{label}/{native_group(p.name)}",
                                                "size": n, "sha": None, "src": p})
    for p in sorted(SILESIA.iterdir()):
        pools["silesia"].append({"rel": f"silesia/{p.name}", "group": f"silesia/{p.name}", "size": p.stat().st_size,
                                 "sha": None, "src": p})
    return pools


def path_verdicts(tool, paths, manifest=False):
    data = "".join(p + "\n" for p in paths).encode("utf-8")
    out = subprocess.run([tool] + (["--manifest"] if manifest else []), input=data, capture_output=True,
                         check=True).stdout.decode("utf-8")
    if manifest:
        return out.strip()
    return {line.split("\t")[1]: line.split("\t")[0] for line in out.splitlines() if line}


def casefold_drop(rows, report):
    """Drop every file and every file under a directory whose casefolded path collides."""
    keys = defaultdict(set)
    for r in rows:
        parts = r["rel"].split("/")
        for i in range(1, len(parts) + 1):
            prefix = "/".join(parts[:i])
            keys[prefix.casefold()].add(prefix)
    bad = {k for k, v in keys.items() if len(v) > 1}
    kept = []
    for r in rows:
        parts = r["rel"].split("/")
        if any("/".join(parts[:i]).casefold() in bad for i in range(1, len(parts) + 1)):
            report.append(r["rel"])
        else:
            kept.append(r)
    return kept


def components(rows):
    """Merge groups that share any content SHA256 into one component (union-find); returns
    group -> component key (the smallest group name), so identical content never splits."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    first = {}
    for r in rows:
        find(r["group"])
        if r["sha"] in first:
            a, b = find(r["group"]), find(first[r["sha"]])
            if a != b:
                parent[max(a, b)] = min(a, b)
        else:
            first[r["sha"]] = r["group"]
    return {g: find(g) for g in parent}


def batches(rows, natural):
    ordered = rows if natural else sorted(rows, key=lambda r: key_hash(r["rel"]))
    if len(ordered) <= 12:
        # Few large files (silesia): the last three files are one append batch each.
        for i, r in enumerate(ordered):
            back = len(ordered) - i
            r["batch"] = "initial" if back > 3 else f"append{4 - back}"
        return ordered
    total = sum(r["size"] for r in ordered)
    cut = [0.7, 0.8, 0.9]
    done = 0
    for r in ordered:
        frac = done / total if total else 0
        r["batch"] = "initial" if frac < cut[0] else "append1" if frac < cut[1] else "append2" if frac < cut[2] else "append3"
        done += r["size"]
    return ordered


def build(tool):
    if LOCAL.joinpath("files").exists() or META.exists() or (META.parent / f"{CORPUS}.json").exists():
        raise SystemExit("corpus exists; use a new corpus ID")
    pools = candidates()
    manifest = {"corpus_id": CORPUS, "created_at": datetime.now(ZoneInfo("America/Los_Angeles")).isoformat(),
                "plan": "research/ideas/seed-append-r1.md (section 7.1)", "machine": {"os": platform.platform()},
                "path_profile": "ADR-0005 via seedarc_path_check (per path and --manifest per split)",
                "batches": "initial = first 70 % of bytes, then append1 / append2 / append3 about 10 % each",
                "pools": {}, "path_rejected": [], "casefold_dropped": [], "skipped": {}}
    skipped = defaultdict(int)
    for pool in ("linux-src", "native-bin", "silesia"):
        rows = [r for r in pools[pool] if r["size"] > 0]
        skipped[f"{pool}:empty"] += len(pools[pool]) - len(rows)
        verdict = path_verdicts(tool, [r["rel"] for r in rows])
        rejected = [r for r in rows if verdict[r["rel"]] != "ok"]
        manifest["path_rejected"] += [[r["rel"], verdict[r["rel"]]] for r in rejected]
        rows = [r for r in rows if verdict[r["rel"]] == "ok"]
        dropped = []
        rows = casefold_drop(rows, dropped)
        manifest["casefold_dropped"] += dropped
        for r in rows:
            if r["sha"] is None:
                r["sha"] = sha(r["src"].read_bytes())
        comp = components(rows)
        units = defaultdict(list)  # component -> rows (whole components move together)
        for r in rows:
            units[comp[r["group"]]].append(r)
        split_rows = {"dev": [], "holdout": []}
        totals = {"dev": 0, "holdout": 0}
        for c in sorted(units, key=key_hash):
            side = "dev" if int(key_hash(c)[0], 16) % 2 == 0 else "holdout"
            size = sum(r["size"] for r in units[c])
            cap = SPLIT_CAPS.get(pool)
            if cap is not None and totals[side] + size > cap:
                continue
            split_rows[side] += units[c]
            totals[side] += size
        manifest["pools"][pool] = write_pool(tool, pool, split_rows, natural=False)
    # wiki-text: enwik8 cut into 100 consecutive parts; first half dev, second half holdout
    data = ENWIK8.read_bytes()
    if len(data) != ENWIK8_PARTS * ENWIK8_PART_BYTES:
        raise SystemExit("unexpected enwik8 size")
    split_rows = {"dev": [], "holdout": []}
    for i in range(ENWIK8_PARTS):
        part = data[i * ENWIK8_PART_BYTES:(i + 1) * ENWIK8_PART_BYTES]
        split_rows["dev" if i < ENWIK8_PARTS // 2 else "holdout"].append(
            {"rel": f"enwik8/part-{i:03d}.xml", "group": f"enwik8/part-{i:03d}", "size": len(part), "sha": sha(part),
             "bytes": part})
    manifest["pools"]["wiki-text"] = write_pool(tool, "wiki-text", split_rows, natural=True)
    manifest["skipped"] = dict(skipped)
    manifest["sources"] = {
        "linux-src": {"source": "linux-6.12.tar.xz, cdn.kernel.org", "sha256": "b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb",
                      "license": "GPL-2.0 (with exceptions per file)", "redistribution": "listing only in this repository"},
        "native-bin": {"source": "MSVC 14.51.36231 lib/x64; MSYS2 UCRT64 bin and lib on this machine",
                       "license": "Microsoft / per package", "redistribution": "local evaluation only; not redistributed"},
        "silesia": {"source": "silesia.zip, sun.aei.polsl.pl", "sha256": "0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af",
                    "license": "mixed per file", "redistribution": "local evaluation only; not redistributed"},
        "wiki-text": {"source": "enwik8 (enwik8.zip, mattmahoney.net)", "sha256_enwik8": "2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8",
                      "license": "CC BY-SA (Wikipedia text)", "note": "cut into 100 consecutive parts of 1,000,000 bytes; constructed to model appended data"},
    }
    manifest["disjointness_check"] = check()
    (META.parent / f"{CORPUS}.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                               encoding="utf-8", newline="\n")
    print("rejected:", len(manifest["path_rejected"]), "casefold dropped:", len(manifest["casefold_dropped"]))
    print("check:", manifest["disjointness_check"])


def write_pool(tool, pool, split_rows, natural):
    entry = {}
    for side, rows in split_rows.items():
        verdict = path_verdicts(tool, [r["rel"] for r in rows], manifest=True)
        if not verdict.startswith("ok"):
            raise SystemExit(f"{pool}/{side}: manifest check failed: {verdict}")
        for r in rows:
            data = r.pop("bytes", None)
            if data is None:
                data = r["src"].read_bytes()
            if len(data) != r["size"] or (r["sha"] and sha(data) != r["sha"]):
                raise SystemExit(f"{r['rel']}: content differs from its listing")
            r["sha"] = sha(data)
            dest = LOCAL / "files" / side / pool / r["rel"]
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        ordered = batches(rows, natural)
        text = "".join(f"{r['rel']}\t{r['size']}\t{r['sha']}\t{r['group']}\t{r['batch']}\n"
                       for r in sorted(ordered, key=lambda r: r["rel"]))
        META.mkdir(parents=True, exist_ok=True)
        (META / f"{side}-{pool}.tsv").write_text(text, encoding="utf-8", newline="\n")
        per_batch = defaultdict(lambda: [0, 0])
        for r in ordered:
            per_batch[r["batch"]][0] += 1
            per_batch[r["batch"]][1] += r["size"]
        entry[side] = {"files": len(rows), "bytes": sum(r["size"] for r in rows),
                       "groups": len({r["group"] for r in rows}), "manifest_check": verdict,
                       "batches": {b: {"files": n, "bytes": s} for b, (n, s) in sorted(per_batch.items())},
                       "order": "natural (part index)" if natural else "sha256('append-r1:<path>')",
                       "listing": f"{CORPUS}/{side}-{pool}.tsv", "listing_sha256": sha(text.encode())}
        print(pool, side, entry[side]["files"], entry[side]["bytes"], entry[side]["groups"],
              {b: v["files"] for b, v in entry[side]["batches"].items()}, flush=True)
    return entry


def check():
    """Raise if any content SHA256 occurs in both dev and holdout (any pool)."""
    seen = {"dev": set(), "holdout": set()}
    files = 0
    for listing in sorted(META.glob("*.tsv")):
        side = listing.name.split("-", 1)[0]
        for line in listing.read_text(encoding="utf-8").splitlines():
            seen[side].add(line.split("\t")[2])
            files += 1
    shared = seen["dev"] & seen["holdout"]
    if shared:
        raise AssertionError(f"{len(shared)} content hashes in both dev and holdout")
    return {"files": files, "dev_hashes": len(seen["dev"]), "holdout_hashes": len(seen["holdout"]), "shared": 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--path-check")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        print(json.dumps(check()))
        return
    if not a.path_check:
        ap.error("--path-check is required to build")
    build(str(Path(a.path_check).resolve(strict=True)))


if __name__ == "__main__":
    main()
