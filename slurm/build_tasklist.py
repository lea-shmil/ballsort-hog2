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
    # Suboptimal: the weight sweep and its w=infinity end. The weights sit just above 1
    # because that is where the interesting region turned out to be -- the triage pass had
    # w=1.25 returning the optimal length on more than half its instances, so anything
    # coarser only measures how bad a badly-weighted search can get.
    "wastar-1.005", "wastar-1.01", "wastar-1.05", "wastar-1.1", "greedy",
    # The paper's own algorithm: serial with their symmetry reduction, serial without it, and
    # with the layer expansion threaded. rscbt-par expands exactly the same nodes as rscbt by
    # construction, so the pair isolates the parallel speedup their §6 motivates.
    "rscbt", "rscbt-nosym", "rscbt-par",
]

# Workers per algorithm. Everything except rscbt-par is serial, so only rscbt-par asks for
# more than one core -- and it gets its own array with a matching --cpus-per-task, rather
# than the whole sweep reserving cores it would not use.
PARALLEL_THREADS = 16
THREADS = {"rscbt-par": PARALLEL_THREADS}

# Memory per algorithm, in GB. Measured, not guessed: over 9,828 runs the algorithms that
# keep a closed list peaked at 15-32 GB, while every other algorithm in the set -- the
# informed searches, rscbt, the weight sweep, iddfs -- stayed under 0.5 GB. Giving all of
# them the same large allocation would cut concurrency by an order of magnitude to no
# purpose, so they are partitioned and submitted as separate arrays.
#
# 60 GB for the closed-list group. The ceiling is set by what the shared cluster will
# actually schedule, not by what the cells want: the cpu128 nodes have 514 GB each but run
# with most of it already allocated to other users (491 of 514 GB on one node when this was
# measured), so a 240 GB request is accepted and then pends on Resources indefinitely. 60 GB
# schedules promptly and still leaves the node headroom -- both for other jobs and for the
# search process itself beyond its closed list.
#
# What that buys, now measured rather than projected. A relabelling pass over the 1,392 killed
# runs separated the two causes (860 timeouts, 532 OOMs) and the split falls exactly along
# algorithm class: the linear-space searches (iddfs, idastar-*) recorded *zero* OOMs and ran out
# of clock instead, while the closed-list searches account for every OOM.
#
# So 60 GB helps far less than a state-count projection suggests, because most of what a
# projection flags as memory-hungry was never dying of memory:
#   - 12x3 is the clear win: dijkstra peaked at 31.4 GB on the instances that finished and OOMed
#     on 20 others, so it sits right on the 32 GB boundary and should clear it at 60 GB.
#   - 13x3 astar-misplaced peaked at 19.8 GB with 8 OOMs, so those should clear too.
#   - 13x3 bfs/dijkstra (60 OOMs) need roughly 164 GB and will not.
#   - 11x3 and 2x7 were pure timeouts with no OOM at all. An earlier note here claimed 60 GB
#     would rescue them; that conflated projected footprint with observed cause of death, which
#     was unknowable until the kill reasons were separated.
# Beyond those, 6x4 needs about 216 GB, 4x5 660 GB, 3x6 1.4 TB, 2x8 3.7 TB and 7x4 7.2 TB.
# Exhaustive search on those cells is out of reach on this cluster, so running out of memory is
# the finding rather than a misconfiguration -- the same thing the paper reports as empty boxes
# in its Figure 6, and the reason every row records the limit it ran under.
CLOSED_LIST_MEMORY_GB = 60
DEFAULT_MEMORY_GB = 8
MEMORY_GB = {
    "bfs": CLOSED_LIST_MEMORY_GB,
    "bibfs": CLOSED_LIST_MEMORY_GB,
    "dijkstra": CLOSED_LIST_MEMORY_GB,
    "frontier-bfs": CLOSED_LIST_MEMORY_GB,
    "astar-misplaced": CLOSED_LIST_MEMORY_GB,
    "astar-paper": CLOSED_LIST_MEMORY_GB,   # also keeps a closed list, though it rarely fills it
}

# Difficulty tiers, from the reachable state count, documented in the README.
#
# Every algorithm is now attempted on every tier. Earlier runs capped the exhaustive searches
# at tier B and IDDFS at tier A to avoid buying thousands of 60-minute timeouts, but a cap
# means the reach of those algorithms is *assumed* rather than measured -- and the triage pass
# showed the assumption was already wrong in one direction, with A*+misplaced clearing 97% of
# tier C. Timeouts cost cluster time; assumed results cost correctness. MAX_TIER stays here,
# empty, so reinstating a cap is a one-line change rather than a rewrite.
TIER_ORDER = {"trivial": 0, "A": 1, "B": 2, "C": 3, "D": 4}
MAX_TIER = {}
DEFAULT_MAX_TIER = "D"


def row_path(rowdir, algorithm, instance, seed):
    """Where experiment_sweep.sbatch writes this task's row. Must match the shell:
       ROW="$ROWDIR/${ALGORITHM}__$(tr '/' '_' <<<"$INSTANCE")__seed${SEED_LABEL}.csv"
    """
    return os.path.join(rowdir, f"{algorithm}__{instance.replace('/', '_')}__seed{seed}.csv")


def already_done(rowdir, algorithm, instance, seed):
    """True only if a *completed* run left a row behind.

    Emptiness is the discriminator, and it matters. The array script opens the row file with
    `>` before run_experiment starts, so a task killed by SLURM for exceeding its time or
    memory limit leaves a zero-byte file behind. Treating that as done would permanently
    retire every timeout from the sweep and, worse, drop it from the task list -- which is
    what aggregate_results.py reads to tell "timed out" from "never attempted". So an empty
    row counts as not done and gets retried.
    """
    path = row_path(rowdir, algorithm, instance, seed)
    try:
        return os.path.getsize(path) > 0
    except OSError:
        return False


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
    ap.add_argument("--skip-existing", action="store_true",
                    help="omit tasks that already have a non-empty row in --rowdir, so a "
                         "scale-up only runs what is missing. Instances are a function of "
                         "(seed, height, tubes, index) and the algorithms are deterministic, "
                         "so an existing row is exactly the row this task would reproduce.")
    ap.add_argument("--rowdir", default="results/rows",
                    help="where completed rows live (default results/rows)")
    args = ap.parse_args()

    seeds = [s.strip() for s in args.seeds.split(",") if s.strip()]
    if not seeds:
        sys.exit("error: --seeds is empty")
    if not os.path.exists(args.binary):
        sys.exit(f"error: {args.binary} not found -- build first")

    os.makedirs(os.path.dirname(args.tasklist) or ".", exist_ok=True)

    rows = []
    skipped = []
    reused = 0
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
                    if args.skip_existing and already_done(args.rowdir, algorithm,
                                                           instance, seed):
                        reused += 1
                        continue
                    rows.append((algorithm, instance, seed,
                                 THREADS.get(algorithm, 1),
                                 MEMORY_GB.get(algorithm, DEFAULT_MEMORY_GB)))

    with open(args.tasklist, "w") as f:
        f.write("# algorithm\tinstance\tseed_label\tthreads\tmemory_gb\n")
        f.write(f"# grid={args.grid} seeds={','.join(seeds)} count={args.count}\n")
        f.write(f"# {len(rows)} tasks, {len(skipped)} (algorithm, instance) pairs "
                f"skipped by tier cap\n")
        if args.skip_existing:
            f.write(f"# {reused} task(s) omitted: a completed row already exists in "
                    f"{args.rowdir}\n")
        # Summarize the skips per (algorithm, cap) with the cells involved, so the record
        # says what was not attempted and why without one line per instance.
        by_algorithm = defaultdict(set)
        for algorithm, cell, tier, cap in skipped:
            by_algorithm[(algorithm, cap)].add(f"{cell}({tier})")
        for (algorithm, cap), cells in sorted(by_algorithm.items()):
            f.write(f"# not attempted: {algorithm} capped at tier {cap}, "
                    f"skipping {' '.join(sorted(cells))}\n")
        for algorithm, instance, seed, threads, memory in rows:
            f.write(f"{algorithm}\t{instance}\t{seed}\t{threads}\t{memory}\n")

    print(f"wrote {len(rows)} tasks to {args.tasklist}")
    print(f"seeds: {', '.join(seeds)}  grid: {args.grid}  instances/cell/seed: {args.count}")
    if skipped:
        print(f"{len(skipped)} pairs skipped by the tier caps "
              f"(use --all to attempt them anyway)")
    if args.skip_existing:
        print(f"{reused} task(s) already have a completed row in {args.rowdir} and were "
              f"omitted")
    chunks = (len(rows)+1000)//1001
    print(f"MaxArraySize is 1001, so this needs {chunks} array submission(s): "
          f"bash slurm/submit_sweep.sh")


if __name__ == "__main__":
    main()
