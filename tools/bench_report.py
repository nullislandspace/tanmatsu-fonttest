#!/usr/bin/env python3
"""Turn captured benchmark runs into comparisons, and regenerate PROGRESS.md.

Deliberately separate from testrun.py: reports regenerate offline, from the JSON
already in results/, with no device attached. Running it twice over unchanged
inputs must produce byte-identical output -- that property is what lets the
generated tables live in git without churning every commit.

Three things it does:

  --compare RUN      one run against the baseline: per-cell deltas, geometric
                     means per group, drift, and the fb_hash correctness gate
  --repeatability    N runs against each other, which is the step-9 gate: until
                     repeated runs of the *same* binary agree, no optimization
                     result is attributable to code
  --progress         rewrite the generated tables in results/PROGRESS.md

Stdlib only.
"""

import argparse
import json
import math
import os
import sys

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_UNSTABLE = 3
EXIT_CORRECTNESS = 4
EXIT_MISMATCH = 5

# Any of these differing between two runs means the numbers are not measuring
# the same thing, so comparing them would be worse than not comparing at all.
COMPARABILITY_KEYS = (
    ("schema", lambda m: m.get("schema")),
    ("opt", lambda m: m.get("build", {}).get("opt")),
    ("corpus_ver", lambda m: m.get("bench", {}).get("corpus_ver")),
    ("tile_dim", lambda m: m.get("mem", {}).get("tile_dim")),
    ("n_samples", lambda m: m.get("bench", {}).get("n_samples")),
    ("target_sample_us", lambda m: m.get("bench", {}).get("target_sample_us")),
    ("join_placement", lambda m: m.get("bench", {}).get("join_placement")),
    ("pax_cfg", lambda m: m.get("cfg", {}).get("pax")),
)

# Reporting groups. The first three are the three distinct pixel loops in PAX,
# which is the split that decides which optimization a change belongs to.
GROUPS = (
    ("fast1", lambda ax: ax["path"] == "blit/direct"),
    ("fast2", lambda ax: ax["path"] == "blit/alpha"),
    ("shader", lambda ax: ax["path"].startswith("shader/")),
    ("565", lambda ax: ax["fmt"] == "565"),
    ("888", lambda ax: ax["fmt"] == "888"),
    ("sram", lambda ax: ax["mem"] == "sram"),
    ("psram", lambda ax: ax["mem"] == "psram"),
    ("sync", lambda ax: ax["rend"] == "sync"),
    ("async", lambda ax: ax["rend"].startswith("async")),
)

MARK_BEGIN = "<!-- generated: {} -->"
MARK_END = "<!-- /generated: {} -->"


def geomean(values):
    """Ratios compose multiplicatively, so the mean of ratios is geometric.

    Averaging speedup percentages arithmetically is a standard way to report a
    win that is not there.
    """
    values = [v for v in values if v > 0]
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def cells_by_id(run):
    return {c["id"]: c for c in run.get("cells", [])}


def comparability(a, b):
    """Return the list of keys on which two runs disagree."""
    ma, mb = a.get("meta", {}) or {}, b.get("meta", {}) or {}
    return [name for name, get in COMPARABILITY_KEYS if get(ma) != get(mb)]


def drift_pct(run):
    """Thermal/ordering drift, from the base cell measured first and last."""
    by = cells_by_id(run)
    first, last = by.get("drift.first"), by.get("drift.last")
    if not first or not last:
        return None
    return (last["d"]["ns_iter"] / first["d"]["ns_iter"] - 1) * 100.0


def compare(base, new):
    """Per-cell and per-group comparison of two runs."""
    base_by, new_by = cells_by_id(base), cells_by_id(new)

    rows = []
    for cid in sorted(set(base_by) | set(new_by)):
        b, n = base_by.get(cid), new_by.get(cid)
        if b is None:
            rows.append({"id": cid, "state": "added", "cell": n})
            continue
        if n is None:
            rows.append({"id": cid, "state": "removed", "cell": b})
            continue

        pct = (n["d"]["ns_iter"] / b["d"]["ns_iter"] - 1) * 100.0
        pct_min = (n["m"]["min"] / n["m"]["reps"]) / (b["m"]["min"] / b["m"]["reps"])
        pct_min = (pct_min - 1) * 100.0
        noise = 2.0 * max(n["q"]["cv"], b["q"]["cv"])
        rows.append({
            "id": cid,
            "state": "matched",
            "cell": n,
            "base": b,
            "pct": pct,
            "pct_min": pct_min,
            "ratio": n["d"]["ns_iter"] / b["d"]["ns_iter"],
            "significant": abs(pct) > noise,
            "noise": noise,
            "hash_ok": n["h"] == b["h"],
            # A cell whose reps differ wildly is not cache-comparable even when
            # the per-iteration numbers look fine.
            "reps_warn": max(n["m"]["reps"], b["m"]["reps"]) > 4 * min(n["m"]["reps"], b["m"]["reps"]),
        })

    matched = [r for r in rows if r["state"] == "matched"]
    groups = {}
    for name, pred in GROUPS:
        sel = [r["ratio"] for r in matched if pred(r["cell"]["ax"])]
        if sel:
            groups[name] = geomean(sel)

    return {
        "rows": rows,
        "matched": matched,
        "overall": geomean([r["ratio"] for r in matched]) if matched else float("nan"),
        "groups": groups,
        "regressions": [r for r in matched if not r["hash_ok"]],
        "added": [r for r in rows if r["state"] == "added"],
        "removed": [r for r in rows if r["state"] == "removed"],
    }


def repeatability(runs):
    """Spread of each cell's median across N runs of the same binary.

    The gate the plan refuses to skip: sync cells within 3%, async within 6%.
    Anything worse and a later "8% win" cannot be told from run-to-run noise.
    """
    per_cell = {}
    for run in runs:
        for cid, cell in cells_by_id(run).items():
            per_cell.setdefault(cid, []).append(cell)

    rows = []
    for cid in sorted(per_cell):
        cells = per_cell[cid]
        if len(cells) < 2:
            continue
        values = [c["d"]["ns_iter"] for c in cells]
        lo, hi = min(values), max(values)
        mid = sorted(values)[len(values) // 2]
        spread = (hi - lo) / mid * 100.0
        is_async = cells[0]["ax"]["rend"].startswith("async")
        limit = 6.0 if is_async else 3.0
        rows.append({
            "id": cid,
            "n": len(cells),
            "spread": spread,
            "limit": limit,
            "pass": spread <= limit,
            "values": values,
            "async": is_async,
        })
    return rows


def fmt_pct(value):
    return f"{value:+.2f}%"


def render_compare(base, new, result, mismatches):
    out = []
    out.append(f"# {new.get('runid', '?')} vs baseline {base.get('runid', '?')}")
    out.append("")
    meta = new.get("meta", {}) or {}
    build = meta.get("build", {})
    out.append(f"- opt `{build.get('opt')}`, pax `{build.get('pax_git')}` "
               f"({build.get('pax_branch')}), app `{build.get('app_git')}`")
    out.append(f"- cells {len(result['matched'])} matched, "
               f"{len(result['added'])} added, {len(result['removed'])} removed")
    dr = drift_pct(new)
    out.append(f"- drift {fmt_pct(dr)}" if dr is not None else "- drift n/a")
    if mismatches:
        out.append(f"- **comparability mismatch**: {', '.join(mismatches)}")
    out.append("")

    out.append("## Aggregate (geometric mean of per-cell ratios, <1 is faster)")
    out.append("")
    out.append("| Group | Ratio | Change |")
    out.append("|---|---|---|")
    out.append(f"| **overall** | {result['overall']:.4f} | {fmt_pct((result['overall'] - 1) * 100)} |")
    for name, _ in GROUPS:
        if name in result["groups"]:
            ratio = result["groups"][name]
            out.append(f"| {name} | {ratio:.4f} | {fmt_pct((ratio - 1) * 100)} |")
    out.append("")

    out.append("## Per cell")
    out.append("")
    out.append("| Cell | Base ns/iter | New ns/iter | Change | Min change | Signif | Hash |")
    out.append("|---|---|---|---|---|---|---|")
    for row in sorted(result["matched"], key=lambda r: r["pct"]):
        out.append("| `{}` | {:,} | {:,} | {} | {} | {} | {} |".format(
            row["id"], row["base"]["d"]["ns_iter"], row["cell"]["d"]["ns_iter"],
            fmt_pct(row["pct"]), fmt_pct(row["pct_min"]),
            "yes" if row["significant"] else "no",
            "ok" if row["hash_ok"] else "**CHANGED**"))
    for row in result["added"]:
        out.append(f"| `{row['id']}` | - | {row['cell']['d']['ns_iter']:,} | added | | | |")
    for row in result["removed"]:
        out.append(f"| `{row['id']}` | {row['cell']['d']['ns_iter']:,} | - | removed | | | |")
    out.append("")
    return "\n".join(out)


def results_table(runs, baselines):
    """One row per run, newest last. This is what PROGRESS.md carries forward."""
    lines = ["| Run | pax commit | Opt | Cells | Overall | fast1 | fast2 | shader | Drift | Correct |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for run in runs:
        meta = run.get("meta", {}) or {}
        build = meta.get("build", {})
        opt = (build.get("opt") or "?")
        base = baselines.get(opt.lower())

        overall = groups = correct = None
        if base is not None and base.get("runid") != run.get("runid"):
            result = compare(base, run)
            overall = result["overall"]
            groups = result["groups"]
            correct = "ok" if not result["regressions"] else f"{len(result['regressions'])} changed"
        elif base is not None:
            overall, correct = 1.0, "baseline"

        dr = drift_pct(run)
        lines.append("| `{}` | `{}` | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            run.get("runid", "?"),
            (build.get("pax_git") or "?")[:12],
            opt,
            len(run.get("cells", [])),
            f"{overall:.4f}" if overall is not None else "-",
            f"{groups['fast1']:.4f}" if groups and "fast1" in groups else "-",
            f"{groups['fast2']:.4f}" if groups and "fast2" in groups else "-",
            f"{groups['shader']:.4f}" if groups and "shader" in groups else "-",
            f"{dr:+.2f}%" if dr is not None else "-",
            correct or "-"))
    return "\n".join(lines)


def headline_table(run):
    """Per-path cost of the most recent run, in absolute terms.

    The ratio tables say whether a change helped; this one says where the time
    actually goes, which is what decides what to work on next.
    """
    cells = [c for c in run.get("cells", []) if c["g"] == "cube"]
    lines = ["| Pixel loop | Cells | Median ns/dest px | Min | Max |",
             "|---|---|---|---|---|"]
    for name, pred in GROUPS[:3]:
        sel = [c for c in cells if pred(c["ax"])]
        if not sel:
            continue
        values = sorted(c["d"]["ns_px"] for c in sel)
        lines.append("| {} | {} | {:,.1f} | {:,.1f} | {:,.1f} |".format(
            name, len(sel), values[len(values) // 2], values[0], values[-1]))
    return "\n".join(lines)


def splice(text, name, body):
    """Replace a marked generated block, or append one if it is not there yet."""
    begin, end = MARK_BEGIN.format(name), MARK_END.format(name)
    block = f"{begin}\n{body}\n{end}"
    if begin in text and end in text:
        head, _, rest = text.partition(begin)
        _, _, tail = rest.partition(end)
        return head + block + tail
    return text.rstrip("\n") + "\n\n" + block + "\n"


def load_runs(runs_dir):
    if not os.path.isdir(runs_dir):
        return []
    paths = sorted(os.path.join(runs_dir, n) for n in os.listdir(runs_dir) if n.endswith(".json"))
    return [load(p) for p in paths]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", default="results")
    parser.add_argument("--compare", metavar="RUN",
                        help="run JSON to compare against its baseline (default: the newest)")
    parser.add_argument("--baseline", help="baseline JSON (default: results/baseline-<opt>.json)")
    parser.add_argument("--repeatability", nargs="+", metavar="RUN",
                        help="check N runs of the same binary against each other")
    parser.add_argument("--progress", action="store_true",
                        help="regenerate the generated tables in results/PROGRESS.md")
    parser.add_argument("--allow-mismatch", action="store_true")
    args = parser.parse_args()

    runs_dir = os.path.join(args.dir, "runs")
    status = EXIT_OK

    if args.repeatability:
        runs = [load(p) for p in args.repeatability]
        rows = repeatability(runs)
        if not rows:
            print("No cells in common across those runs.", file=sys.stderr)
            return EXIT_USAGE
        failed = [r for r in rows if not r["pass"]]
        worst = max(rows, key=lambda r: r["spread"])
        print(f"Repeatability over {len(runs)} runs, {len(rows)} cells")
        print(f"  worst: {worst['id']} {worst['spread']:.2f}% (limit {worst['limit']:.0f}%)")
        print(f"  median spread: {sorted(r['spread'] for r in rows)[len(rows) // 2]:.2f}%")
        print(f"  over limit: {len(failed)}")
        for row in sorted(failed, key=lambda r: -r["spread"])[:20]:
            print(f"    {row['id']:<38} {row['spread']:6.2f}%  limit {row['limit']:.0f}%  {row['values']}")
        if failed:
            print("GATE FAILED: repeated runs of the same binary do not agree. "
                  "No optimization result would be attributable to code.", file=sys.stderr)
            status = EXIT_UNSTABLE
        else:
            print("GATE PASSED")
        if not args.progress:
            return status

    compare_run = None
    if args.compare or args.progress:
        if args.compare:
            compare_run = load(args.compare)
        else:
            all_runs = load_runs(runs_dir)
            compare_run = all_runs[-1] if all_runs else None

    if args.compare and compare_run is not None:
        opt = (compare_run.get("meta", {}) or {}).get("build", {}).get("opt", "os").lower()
        baseline_path = args.baseline or os.path.join(args.dir, f"baseline-{opt}.json")
        if not os.path.exists(baseline_path):
            print(f"No baseline at {baseline_path}; run 'make testbaseline' first.", file=sys.stderr)
            return EXIT_USAGE

        base = load(baseline_path)
        mismatches = comparability(base, compare_run)
        if mismatches and not args.allow_mismatch:
            print(f"Refusing to compare: {', '.join(mismatches)} differ. "
                  f"Pass --allow-mismatch to override.", file=sys.stderr)
            return EXIT_MISMATCH

        result = compare(base, compare_run)
        detail_dir = os.path.join(args.dir, "detail")
        os.makedirs(detail_dir, exist_ok=True)
        detail_path = os.path.join(detail_dir, f"{compare_run.get('runid', 'run')}.md")
        with open(detail_path, "w", encoding="utf-8") as handle:
            handle.write(render_compare(base, compare_run, result, mismatches) + "\n")
        print(f"  wrote {detail_path}")
        print(f"  overall {result['overall']:.4f} "
              f"({(result['overall'] - 1) * 100:+.2f}%), "
              f"{len(result['regressions'])} framebuffer hashes changed")
        if result["regressions"]:
            for row in result["regressions"][:20]:
                print(f"    CORRECTNESS: {row['id']} "
                      f"{row['base']['h']} -> {row['cell']['h']}")
            status = EXIT_CORRECTNESS

    if args.progress:
        runs = load_runs(runs_dir)
        baselines = {}
        for name in sorted(os.listdir(args.dir)) if os.path.isdir(args.dir) else []:
            if name.startswith("baseline-") and name.endswith(".json"):
                baselines[name[len("baseline-"):-len(".json")]] = load(os.path.join(args.dir, name))

        progress_path = os.path.join(args.dir, "PROGRESS.md")
        text = open(progress_path, encoding="utf-8").read() if os.path.exists(progress_path) else "# Progress\n"
        text = splice(text, "results", results_table(runs, baselines))
        if runs:
            text = splice(text, "headline", headline_table(runs[-1]))
        with open(progress_path, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"  wrote {progress_path}")

    if not (args.compare or args.progress or args.repeatability):
        parser.print_help()
        return EXIT_USAGE

    return status


if __name__ == "__main__":
    sys.exit(main())
