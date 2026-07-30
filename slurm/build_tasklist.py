#!/usr/bin/env python3
"""Generate the multi-seed instance sets and write the sweep's task list.

One line per (algorithm, instance) pair, which is one SLURM array task, which is one CSV
row. Nothing here runs a search -- this is cheap enough for the login node.

Why several seeds. Every algorithm in the comparison is deterministic: the same instance
gives byte-identical node counts on every run. So repetition has to vary the *instances*,
not re-run a fixed one. Each seed produces its own set of 10 instances per cell (the
paper's protocol), and aggregation pools them, so 3 seeds means 30 samples per cell drawn
from the same random-instance distribution. Wall-clock is the only genuinely noisy metric
and is repeated inside run_experiment instead (--timing-repeats, median kept).

Because instance i of a cell is seeded from (seed, height, tubes, i), seeds are independent
and instances 0..9 of the first seed stay the subset directly comparable to the paper's ten.

Usage:
  slurm/build_tasklist.py [--grid core] [--seeds 20250726,11,12] [--count 10]
                          [--outdir instances] [--tasklist results/tasks.tsv] [--all]
"""

import argparse
import csv
import os
import subprocess
import sys
from collections import defaultdict

# The algorithm set. Names must match run_experiment's registry.
ALGORITHMS = [
    # zero-heuristic end of the family
    "bfs", "bibfs", "frontier-bfs", "iddfs", "dijkstra",
    # informed, with our misplaced-ball heuristic and with the paper's DFVS bound
    "astar-misplaced", "astar-paper",
    "idastar-misplaced", "idastar-paper",
    # suboptimal: the weight sweep and its w=infinity end
    "wastar-1.25", "wastar-1.5", "wastar-2", "wastar-3", "wastar-5", "greedy",
    # the paper's own algorithm, faithful (symmetry reduction on) and without it
    "rscbt", "rscbt-nosym",
]

# Highest difficulty tier each algorithm is attempted on by default. The tiers come from the
# reachable state count and are documented in the README; the point of the cap is that a
# 60-minute timeout is expensive to buy 8000 times over for runs whose outcome is already
# known. --all removes the cap and attempts everything.
#
# Skipped pairs are written to the task list as comments, so "not attempted" stays visible
# in the record rather than looking like missing data.
TIER_ORDER = {"trivial": 0, "A": 1, "B": 2, "C": 3, "D": 4}
MAX_TIER = {
    "iddfs": "A",            # no closed list; 161M expansions on 3x4 already
    "bfs": "B",
    "bibfs": "B",
    "frontier-bfs": "B",
    "dijkstra": "B",
}
DEFAULT_MAX_TIER = "D"       # informed algorithms and rscbt are attempted everywhere


def generate(binary, grid, seed, count, outdir):
    """Generate one seed's instance tree. Returns the manifest path."""
    target = os.path.join(outdir, f"seed-{seed}")
    cmd = [binary, "--grid", grid, "--outdir", target,
           "--seed", str(seed), "--count", str(count)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(target, "manifest.csv")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--grid", default="core",
                    help="cell set: core (default), runnable, or paper")
    ap.add_argument("--seeds", default="20250726,20250727,20250728",
                    help="comma-separated instance-set seeds (default: three)")
    ap.add_argument("--count", type=int, default=10,
                    help="instances per cell per seed (default 10, the paper's protocol)")
    ap.add_argument("--outdir", default="instances")
    ap.add_argument("--tasklist", default="results/tasks.tsv")
    ap.add_argument("--binary", default="./build/generate_instances")
    ap.add_argument("--all", action="store_true",
                    help="attempt every algorithm on every cell, ignoring the tier caps")
    args = ap.parse_args()

    seeds = [s.strip() for s in args.seeds.split(",") if s.strip()]
    if not seeds:
        sys.exit("error: --seeds is empty")
    if not os.path.exists(args.binary):
        sys.exit(f"error: {args.binary} not found -- build first")

    os.makedirs(os.path.dirname(args.tasklist) or ".", exist_ok=True)

    rows = []
    skipped = []
    for seed in seeds:
        manifest = generate(args.binary, args.grid, seed, args.count, args.outdir)
        with open(manifest) as f:
            for entry in csv.DictReader(f):
                instance = os.path.join(args.outdir, f"seed-{seed}", entry["file"])
                tier = entry["tier"]
                for algorithm in ALGORITHMS:
                    cap = MAX_TIER.get(algorithm, DEFAULT_MAX_TIER)
                    if not args.all and TIER_ORDER[tier] > TIER_ORDER[cap]:
                        skipped.append((algorithm, entry["cell"], tier, cap))
                        continue
                    rows.append((algorithm, instance, seed))

    with open(args.tasklist, "w") as f:
        f.write("# algorithm\tinstance\tseed_label\n")
        f.write(f"# grid={args.grid} seeds={','.join(seeds)} count={args.count}\n")
        f.write(f"# {len(rows)} tasks, {len(skipped)} (algorithm, instance) pairs "
                f"skipped by tier cap\n")
        # Summarize the skips per (algorithm, cap) with the cells involved, so the record
        # says what was not attempted and why without one line per instance.
        by_algorithm = defaultdict(set)
        for algorithm, cell, tier, cap in skipped:
            by_algorithm[(algorithm, cap)].add(f"{cell}({tier})")
        for (algorithm, cap), cells in sorted(by_algorithm.items()):
            f.write(f"# not attempted: {algorithm} capped at tier {cap}, "
                    f"skipping {' '.join(sorted(cells))}\n")
        for algorithm, instance, seed in rows:
            f.write(f"{algorithm}\t{instance}\t{seed}\n")

    print(f"wrote {len(rows)} tasks to {args.tasklist}")
    print(f"seeds: {', '.join(seeds)}  grid: {args.grid}  instances/cell/seed: {args.count}")
    if skipped:
        print(f"{len(skipped)} pairs skipped by the tier caps "
              f"(use --all to attempt them anyway)")
    chunks = (len(rows)+1000)//1001
    print(f"MaxArraySize is 1001, so this needs {chunks} array submission(s): "
          f"bash slurm/submit_sweep.sh")


if __name__ == "__main__":
    main()
