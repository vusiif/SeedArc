import hashlib, json, subprocess, shutil
from pathlib import Path
root = Path(r"C:\Users\IceLolly\Music\Clion\SeedArc")
run = root / "research/experiments/20261112-b2-adaptive-main-001"
tool = root / "build/msvc-vs2026-release/research/tools/Release/seedarc_seed_archive.exe"
inputs = root / "research/local/20261112-b2-adaptive-main-001/inputs"
out = Path(__file__).parent / "recheck"
shutil.rmtree(out, ignore_errors=True); out.mkdir()
raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
results = []
for r in raw:
    if r["repeat"] != 0 or r["mode"] not in ("zstd3", "lzma6", "oracle-file", "adaptive-0", "adaptive-256k",
                                              "adaptive-1m", "adaptive-unbounded"):
        continue
    cmd = r["commands"][0]
    archive = out / f"{r['dataset']}-{r['mode']}.sarc"
    p = subprocess.run([str(tool), "compress", cmd[2], str(inputs / r["dataset"]), str(archive), cmd[5]], capture_output=True)
    assert p.returncode == 0, p.stderr
    j = json.loads(p.stdout)
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    dec = lambda x: None if x["selector"] is None else [(f["file_key"], f["selected_codec"], f["selected_level"],
              f["decision_reason"], f["sampled_blocks"], [(t["trial_codec"], t["trial_level"], t["trial_output_bytes"]) for t in f["trials"]])
              for f in x["selector"]["files"]]
    same = sha == r["archive_sha256"] and dec(j) == dec(r["result"])
    results.append({"dataset": r["dataset"], "mode": r["mode"], "archive_sha256_equal": sha == r["archive_sha256"],
                    "decisions_and_trials_equal": dec(j) == dec(r["result"])})
    print(r["dataset"], r["mode"], same)
tool_sha = hashlib.sha256(tool.read_bytes()).hexdigest()
(run / "final-tool-recheck.json").write_text(json.dumps({"tool_sha256": tool_sha, "note":
    "post-run rebuild after a behaviour-neutral FileDecision initialization fix (clang -Wmissing-designated-field-initializers); repetition-0 commands of the 7 P0-C modes rerun with the final tool",
    "all_equal": all(x["archive_sha256_equal"] and x["decisions_and_trials_equal"] for x in results),
    "cases": results}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
shutil.rmtree(out)
print("tool", tool_sha)
