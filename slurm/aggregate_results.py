#!/usr/bin/env python3
"""Pool the sweep's per-run rows into per-cell results, and check the correctness contract.

Reads results/rows/*.csv (one row per array task) and writes:

  results/all_runs.csv        every run, concatenated
  results/per_cell.csv        one row per (algorithm, cell): the aggregate
  results/contract.txt        the optimal-length cross-check

Aggregation follows the paper: the reported value per (algorithm, cell) is the **median**
over instances, which is what their §7 does over their ten runs. Here the pool is
(seeds x instances-per-cell) -- 30 instances for three seeds -- because our repetition varies
the instance, not the run: the algorithms are deterministic, so re-running a fixed instance
reproduces its node counts exactly. Wall-clock was already median-of-3 inside each run.

Timeouts are first-class. A task killed by SLURM for exceeding its time or memory limit
leaves no row, so a missing (algorithm, instance) pair is a timeout, not missing data. The
task list is consulted to tell "timed out" apart from "never attempted" (the tier caps in
build_tasklist.py skip pairs whose outcome is already known). Their Figure 6 shows the same
thing as empty boxes.

The correctness contract from CLAUDE.md: every optimal algorithm must return the same
solution length on every instance. That is the evidence the domain, the heuristic and the
ported bound are all correct, and it is checked here because one run only ever sees one
algorithm.

Usage:
  slurm/aggregate_results.py [--rows results/rows] [--tasklist results/tasks.tsv]
                             [--outdir results]
"""

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

# Algorithms required to agree on the optimal length. Weighted A* and greedy best-first are
# exempt by design -- they return lengths >= optimal -- and rscbt variants are optimal, so
# they are in.
OPTIMAL_ALGORITHMS = {
    "bfs", "bibfs", "frontier-bfs", "iddfs", "dijkstra",
    "astar-misplaced", "astar-paper", "idastar-misplaced", "idastar-paper",
    "rscbt", "rscbt-nosym",
}

NUMERIC = ["solution_length", "nodes_expanded", "nodes_generated",
           "max_elements_in_memory", "runtime_seconds", "peak_rss_kb",
           "iterations", "root_lower_bound"]


def load_rows(rowdir):
    rows = []
    for path in sorted(glob.glob(os.path.join(rowdir, "*.csv"))):
        with open(path) as f:
            text = f.read().strip()
        if not text:
            continue                    # killed before it wrote anything
        # Rows are written without a header (run_experiment only emits one on --header).
        for line in text.splitlines():
            if line.startswith("algorithm,"):
                continue
            parts = line.split(",")
            if len(parts) != 17:
                print(f"warning: {path}: expected 17 fields, got {len(parts)} -- skipped",
                      file=sys.stderr)
                continue
            row = {
                "algorithm": parts[0], "cell": parts[1],
                "colors": int(parts[2]), "height": int(parts[3]),
                "instance": int(parts[4]), "seed_label": parts[5],
                "solved": int(parts[6]), "solution_length": int(parts[7]),
                "nodes_expanded": int(parts[8]), "nodes_generated": int(parts[9]),
                "max_elements_in_memory": int(parts[10]),
                "runtime_seconds": float(parts[11]), "peak_rss_kb": int(parts[12]),
                "iterations": int(parts[13]), "root_lower_bound": int(parts[14]),
                "node": parts[15], "cpu_model": parts[16],
            }
            rows.append(row)
    return rows


def load_tasklist(path):
    """Every (algorithm, instance, seed) the sweep was supposed to attempt."""
    attempted = set()
    if not os.path.exists(path):
        return attempted
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                attempted.add((parts[0], parts[1], parts[2]))
    return attempted


def instance_key(row):
    return (row["cell"], row["seed_label"], row["instance"])


def check_contract(rows, out_path):
    """Every optimal algorithm must agree on the length, per instance."""
    by_instance = defaultdict(dict)
    for row in rows:
        if row["algorithm"] in OPTIMAL_ALGORITHMS and row["solved"]:
            by_instance[instance_key(row)][row["algorithm"]] = row["solution_length"]

    disagreements = []
    checked = 0
    for key, lengths in sorted(by_instance.items()):
        distinct = set(lengths.values())
        if len(distinct) > 1:
            disagreements.append((key, dict(sorted(lengths.items()))))
        elif len(lengths) > 1:
            checked += 1

    # Suboptimal algorithms must be >= optimal, and weighted A* within its w bound.
    optimal_of = {}
    for key, lengths in by_instance.items():
        if len(set(lengths.values())) == 1:
            optimal_of[key] = next(iter(lengths.values()))

    bound_violations = []
    for row in rows:
        algorithm = row["algorithm"]
        if not row["solved"] or algorithm in OPTIMAL_ALGORITHMS:
            continue
        optimal = optimal_of.get(instance_key(row))
        if optimal is None or optimal <= 0:
            continue
        length = row["solution_length"]
        if length < optimal:
            bound_violations.append((algorithm, instance_key(row), length, optimal,
                                     "shorter than optimal"))
        elif algorithm.startswith("wastar-"):
            weight = float(algorithm[len("wastar-"):])
            if length > weight*optimal + 1e-9:
                bound_violations.append((algorithm, instance_key(row), length, optimal,
                                         f"exceeds w*optimal = {weight*optimal:g}"))

    with open(out_path, "w") as f:
        f.write("optimal-length agreement\n")
        f.write("========================\n")
        f.write(f"instances with >1 optimal algorithm solved: {checked + len(disagreements)}\n")
        f.write(f"instances where they all agree:              {checked}\n")
        f.write(f"instances where they disagree:               {len(disagreements)}\n\n")
        if disagreements:
            f.write("DISAGREEMENTS -- the domain, a heuristic, or the ported bound is wrong:\n")
            for (cell, seed, index), lengths in disagreements:
                f.write(f"  {cell} seed={seed} instance={index}: {lengths}\n")
            f.write("\n")
        f.write("suboptimal-algorithm bounds\n")
        f.write("===========================\n")
        f.write(f"violations: {len(bound_violations)}\n")
        for algorithm, (cell, seed, index), length, optimal, why in bound_violations:
            f.write(f"  {algorithm} on {cell} seed={seed} instance={index}: "
                    f"length={length} optimal={optimal} -- {why}\n")

    return len(disagreements), len(bound_violations), checked


def aggregate(rows, attempted, outdir):
    # Keyed on the cell too: instance index 0 exists in every cell, so a key without the
    # cell makes one completed run look like a completed run everywhere.
    completed = {(r["algorithm"], r["cell"], r["instance"], r["seed_label"]) for r in rows}

    # Timeouts, per (algorithm, cell): attempted but no row came back.
    cell_of_instance = {}
    for row in rows:
        cell_of_instance[row["instance"]] = row["cell"]

    timeouts = defaultdict(int)
    attempts = defaultdict(int)
    for algorithm, instance_path, seed in attempted:
        # instance path looks like instances/seed-S/{cell}/random_generated_{cell}_{i}.in
        parts = instance_path.split(os.sep)
        cell = parts[-2] if len(parts) >= 2 else "?"
        attempts[(algorithm, cell)] += 1
        index = -1
        base = os.path.basename(instance_path)
        if base.endswith(".in") and "_" in base:
            try:
                index = int(base[base.rfind("_")+1:-3])
            except ValueError:
                index = -1
        if (algorithm, cell, index, seed) not in completed:
            timeouts[(algorithm, cell)] += 1

    groups = defaultdict(list)
    for row in rows:
        groups[(row["algorithm"], row["cell"])].append(row)

    optimal_of = {}
    by_instance = defaultdict(dict)
    for row in rows:
        if row["algorithm"] in OPTIMAL_ALGORITHMS and row["solved"]:
            by_instance[instance_key(row)][row["algorithm"]] = row["solution_length"]
    for key, lengths in by_instance.items():
        if len(set(lengths.values())) == 1:
            optimal_of[key] = next(iter(lengths.values()))

    per_cell = os.path.join(outdir, "per_cell.csv")
    with open(per_cell, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "algorithm", "cell", "colors", "height", "instances_solved",
            "instances_attempted", "timeouts", "coverage",
            "median_solution_length", "median_nodes_expanded", "median_nodes_generated",
            "median_max_elements_in_memory", "median_runtime_seconds",
            "median_peak_rss_kb", "median_iterations",
            "median_suboptimality_ratio", "max_suboptimality_ratio",
        ])
        for (algorithm, cell), group in sorted(groups.items()):
            solved = [r for r in group if r["solved"]]
            n_attempted = attempts.get((algorithm, cell), len(group))
            n_timeout = timeouts.get((algorithm, cell), 0)

            def med(field, source=None):
                values = [r[field] for r in (source if source is not None else solved)]
                return statistics.median(values) if values else ""

            ratios = []
            for r in solved:
                optimal = optimal_of.get(instance_key(r))
                if optimal and optimal > 0:
                    ratios.append(r["solution_length"]/optimal)

            w.writerow([
                algorithm, cell, group[0]["colors"], group[0]["height"],
                len(solved), n_attempted, n_timeout,
                f"{len(solved)/n_attempted:.3f}" if n_attempted else "",
                med("solution_length"), med("nodes_expanded"), med("nodes_generated"),
                med("max_elements_in_memory"), f"{med('runtime_seconds'):.6f}" if solved else "",
                med("peak_rss_kb"), med("iterations"),
                f"{statistics.median(ratios):.4f}" if ratios else "",
                f"{max(ratios):.4f}" if ratios else "",
            ])
    return per_cell, sum(timeouts.values()), sum(attempts.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", default="results/rows")
    ap.add_argument("--tasklist", default="results/tasks.tsv")
    ap.add_argument("--outdir", default="results")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    rows = load_rows(args.rows)
    if not rows:
        sys.exit(f"error: no rows found in {args.rows}")
    attempted = load_tasklist(args.tasklist)

    all_runs = os.path.join(args.outdir, "all_runs.csv")
    with open(all_runs, "w", newline="") as f:
        w = csv.writer(f)
        header = ["algorithm", "cell", "colors", "height", "instance", "seed_label",
                  "solved"] + NUMERIC + ["node", "cpu_model"]
        w.writerow(header)
        for row in sorted(rows, key=lambda r: (r["cell"], r["algorithm"],
                                               r["seed_label"], r["instance"])):
            w.writerow([row[k] for k in header])

    contract = os.path.join(args.outdir, "contract.txt")
    disagreements, violations, checked = check_contract(rows, contract)
    per_cell, n_timeouts, n_attempted = aggregate(rows, attempted, args.outdir)

    # Timing comparability check. runtime_seconds is one of the paper's four metrics, and it
    # is only comparable across rows from the same hardware. The sweep pins itself to one
    # machine type; this is what verifies that actually happened rather than assuming it.
    cpu_models = sorted({r["cpu_model"] for r in rows})
    nodes_used = sorted({r["node"] for r in rows})

    seeds = sorted({r["seed_label"] for r in rows})
    cells = sorted({r["cell"] for r in rows})
    algorithms = sorted({r["algorithm"] for r in rows})

    print(f"{len(rows)} runs: {len(algorithms)} algorithms x {len(cells)} cells "
          f"x {len(seeds)} seed(s) {seeds}")
    print(f"wrote {all_runs}")
    print(f"wrote {per_cell}")
    print(f"wrote {contract}")
    if n_attempted:
        print(f"timeouts / killed: {n_timeouts} of {n_attempted} attempted")
    print(f"optimal-length agreement: {checked} instance(s) cross-checked, "
          f"{disagreements} disagreement(s)")
    print(f"suboptimal-bound violations: {violations}")
    print(f"ran on {len(nodes_used)} node(s), {len(cpu_models)} CPU model(s): "
          f"{'; '.join(cpu_models)}")
    if len(cpu_models) > 1:
        print("WARNING: rows span more than one CPU model -- node counts and solution "
              "lengths are unaffected, but runtime_seconds is not comparable across them. "
              "Re-run the sweep pinned to one machine type (--constraint).")

    if disagreements or violations:
        print("\nCONTRACT VIOLATED -- see contract.txt. Do not report these numbers.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
