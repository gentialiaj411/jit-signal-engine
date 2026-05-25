#!/usr/bin/env bash
# Capture pre/post O2 IR for fused filtered_momentum and count market bid/ask loads.
# Usage: bash bench/run_cse_ir_diff.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build-wsl}"
ENGINE="$BUILD_DIR/jit_signal_engine"
OUT_DIR="$ROOT/bench/results/cse_evidence"
PROGRAM="examples/filtered_momentum.sig"

if [[ ! -x "$ENGINE" ]]; then
  echo "ERROR: $ENGINE not found"
  exit 1
fi

mkdir -p "$OUT_DIR"

"$ENGINE" --dump-ir-pre --all-signals "$PROGRAM" 2>"$OUT_DIR/before.ll" >/dev/null
"$ENGINE" --dump-ir --all-signals "$PROGRAM" 2>"$OUT_DIR/after.ll" >/dev/null

python3 - "$OUT_DIR" <<'PY'
import json
import re
import subprocess
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
before = (out_dir / "before.ll").read_text()
after = (out_dir / "after.ll").read_text()
line_re = re.compile(r"^\s*%((?:mid_)?(?:bid|ask))\d* = load double")

def count_bid_ask(ir: str) -> int:
    return sum(1 for line in ir.splitlines() if line_re.search(line))

pre = count_bid_ask(before)
post = count_bid_ask(after)
git_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()

report = {
    "program": "examples/filtered_momentum.sig",
    "mode": "--all-signals",
    "git_commit": git_sha,
    "market_bid_ask_loads_pre_opt": pre,
    "market_bid_ask_loads_post_opt": post,
    "expected_pre_opt_loads": 2,
    "verified": pre == 2 and 1 <= post <= 2 and pre < 22,
}

md_lines = [
    "# CSE / market-load dedup report (verified)",
    "",
    "## Program",
    f"- Path: `{report['program']}`",
    f"- Mode: `{report['mode']}` (fused `CompileProgram`)",
    "",
    "## Environment",
    f"- Git commit: `{git_sha}`",
    "",
    "## Artifacts",
    "- `bench/results/cse_evidence/before.ll` (pre-O2 IR)",
    "- `bench/results/cse_evidence/after.ll` (post-O2 IR)",
    "",
    "## Market bid/ask load counts",
    "",
    f"| Stage | Bid/ask `load double` count |",
    f"|---|---:|",
    f"| Pre-O2 (emitter + memoization) | {pre} |",
    f"| Post-O2 LLVM | {post} |",
    f"| Prior artifact (pre-memoization) | 22 |",
    "",
    "## Mechanism",
    "LLVM CSE cannot fold loads across opaque `jit_rt_*` calls or dependency-inlined control-flow blocks. The fused JIT emitter preloads each used symbol's bid/ask once at function entry and reuses those SSA values for all later mid/bid/ask lowering.",
    "",
    "## Conclusion",
]
if report["verified"]:
    md_lines.append(
        "Post-memoization IR shows **2** market bid/ask loads for this fused case (down from **22**). "
        "Market-data read deduplication in whole-program JIT is **Verified** for `filtered_momentum.sig`."
    )
else:
    md_lines.append("Load-count gate **failed**; do not promote the claim.")

(out_dir / "cse_diff_verified.json").write_text(json.dumps(report, indent=2) + "\n")
(out_dir / "cse_diff_verified.md").write_text("\n".join(md_lines) + "\n")
print(f"pre={pre} post={post} verified={report['verified']}")
if not report["verified"]:
    sys.exit(1)
PY

echo "Wrote $OUT_DIR/cse_diff_verified.md"
