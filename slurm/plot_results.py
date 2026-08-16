#!/usr/bin/env python3
"""Draw the paper's two figures from the sweep's aggregated results.

Reads what aggregate_results.py wrote (and, for the failure split, the sweep logs)
and emits print-ready PDF plus PNG:

  paper/figures/fig_expansions_vs_states.{pdf,png}
      Median nodes expanded against the reachable configuration count N, log-log,
      one line per representative algorithm. This is the figure that carries the
      paper's main claim: the DFVS-bounded searches stay nearly flat as the state
      space grows by orders of magnitude, while the uninformed ones climb and then
      stop at the cell where they run out of budget.

  paper/figures/fig_coverage.{pdf,png}
      Per-algorithm run outcomes as horizontal stacked bars: solved, killed by
      timeout, killed by the OOM killer. Folds the coverage table and the failure
      table into one panel, so the linear-space / closed-list split is visible at
      a glance rather than by cross-referencing two tables.

It also prints (--numbers, on by default) the individual figures the paper quotes
in prose, so they can be filled in from measurement rather than inferred.

Usage:
  slurm/plot_results.py [--rows results/all_runs.csv] [--cells results/per_cell.csv]
                        [--logs logs/sweep] [--outdir paper/figures]
                        [--no-numbers] [--format pdf,png]

Requires matplotlib (see environment.yml). Everything else is the standard library.
"""

import argparse
import csv
import glob
import math
import os
import re
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")           # headless: this runs on a compute node, not a desktop
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

# ---------------------------------------------------------------------------
# Palette and type
# ---------------------------------------------------------------------------
#
# Colors are the first slots of a validated categorical palette, taken in fixed
# slot order rather than assigned per figure, so an algorithm keeps its hue across
# both figures. The ordering is the colorblind-safety mechanism: these slots were
# checked with a CVD validator on the adjacent-pair list (worst adjacent CVD dE 9.1,
# worst normal-vision dE 19.6 against a near-white surface), and the first three
# additionally clear the all-pairs gate, which is what figure 2's stacked segments
# need. Do not reorder or extend past slot 6 without re-validating.
#
# Three of these sit below 3:1 contrast on a white page, so both figures carry a
# legend and figure 1 varies marker shape as well as hue: identity is never left to
# color alone, which also keeps the figures readable when the paper is printed in
# grayscale.
SURFACE     = "#ffffff"         # the page, not a chart card
INK         = "#0b0b0b"
INK_MUTED   = "#52514e"
GRID        = "#e1e0d9"
AXIS        = "#c3c2b7"

SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]

# Figure 1 series, in slot order. One representative per class rather than all 17:
# fourteen lines on one log-log panel is a spaghetti chart, and the classes are what
# the argument is about. Marker shape is the secondary encoding.
FIG1_SERIES = [
    ("iddfs",           "IDDFS",              SLOT[0], "o"),
    ("bfs",             "BFS",                SLOT[1], "s"),
    ("astar-misplaced", "A* ($h_{mis}$)",     SLOT[2], "^"),
    ("rscbt-nosym",     "Algorithm 1",        SLOT[3], "D"),
    ("astar-paper",     "A* ($h_{DFVS}$)",    SLOT[4], "v"),
    ("greedy",          "Greedy",             SLOT[5], "P"),
]

# Figure 2 outcome classes. Nominal outcome identity, not a severity ladder, so these
# are categorical slots rather than status tokens; slots 1-3 clear the all-pairs gate.
OUTCOME = [
    ("solved",  "solved",         SLOT[0]),
    ("timeout", "killed: timeout", SLOT[1]),
    ("oom",     "killed: OOM",     SLOT[2]),
]

# The paper is set in Times; matching it keeps the figures from reading as pasted in.
# Falls back cleanly when Times is absent, which it usually is on a Linux cluster.
plt.rcParams.update({
    "font.family":       "serif",
    "font.serif":        ["Times New Roman", "Nimbus Roman", "DejaVu Serif"],
    "font.size":         8,
    "axes.labelsize":    8,
    "axes.titlesize":    8,
    "legend.fontsize":   7,
    "xtick.labelsize":   7,
    "ytick.labelsize":   7,
    "axes.edgecolor":    AXIS,
    "axes.linewidth":    0.6,
    "grid.color":        GRID,
    "grid.linewidth":    0.5,
    "figure.facecolor":  SURFACE,
    "axes.facecolor":    SURFACE,
    "savefig.facecolor": SURFACE,
    "savefig.bbox":      "tight",
    "savefig.pad_inches": 0.02,
    "pdf.fonttype":      42,    # embed TrueType, not Type 3: many venues reject Type 3
    "ps.fonttype":       42,
})

AAAI_COL   = 3.3    # inches, one AAAI column
AAAI_FULL  = 7.0    # inches, both columns (\begin{figure*})


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def reachable_state_count(num_colors, tube_height):
    """The paper's N = (h+c)!(hc)! / (c! (h!)^(c+1)), as ReachableStateCount does it.

    Same two factors: C(h+c, c) ways to place the h empty slots over the c+1 tubes,
    times (hc)!/(h!)^c orderings of the ball multiset along the remaining slots.
    Exact in Python integers, so no precision caveat applies at any grid size.
    """
    c, h = num_colors, tube_height
    return math.comb(h + c, c) * math.factorial(h * c) // (math.factorial(h) ** c)


def read_csv_rows(path):
    if not os.path.exists(path):
        sys.exit(f"error: {path} not found -- run slurm/aggregate_results.py first")
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def to_float(value):
    """per_cell.csv leaves a median empty when a cell had no solved run."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def load_kill_reasons(logdir):
    """Count timeouts and OOM kills per algorithm from the sweep logs.

    experiment_sweep.sbatch distinguishes the two deliberately -- it uses a plain
    SIGTERM timeout so that exit 124 means the clock ran out and only exit 137 means
    the OOM killer -- and writes one line per kill:

        KILLED reason=timeout        elapsed=..s limit=..s  algorithm=A instance=I seed=S
        KILLED reason=out-of-memory  elapsed=..s limit=..MB algorithm=A instance=I seed=S
        KILLED reason=failed status=N elapsed=..s           algorithm=A instance=I seed=S

    Returns {algorithm: {"timeout": n, "oom": n, "failed": n}}, empty if no logs are
    present -- in which case figure 2 falls back to one undifferentiated killed bar
    and says so, rather than silently reporting zero OOMs.
    """
    counts = defaultdict(lambda: defaultdict(int))
    pattern = re.compile(
        r"KILLED\s+reason=(?P<reason>\S+).*?\balgorithm=(?P<algorithm>\S+)")
    files = sorted(glob.glob(os.path.join(logdir, "*.out")))
    seen = set()
    for path in files:
        with open(path, errors="replace") as f:
            for line in f:
                m = pattern.search(line)
                if not m:
                    continue
                # A rerun can leave the same kill in two log files; key on the whole
                # identifying tail so a re-submitted chunk is not counted twice.
                key = line.strip()
                if key in seen:
                    continue
                seen.add(key)
                reason = m.group("reason")
                bucket = {"timeout": "timeout",
                          "out-of-memory": "oom"}.get(reason, "failed")
                counts[m.group("algorithm")][bucket] += 1
    return counts, len(files)


# ---------------------------------------------------------------------------
# Figure 1
# ---------------------------------------------------------------------------

def figure_expansions(cells, outdir, formats):
    """Median nodes expanded against N, log-log, one line per representative algorithm."""
    # cell -> N, and the per-(algorithm, cell) medians
    by_algorithm = defaultdict(list)
    for row in cells:
        expanded = to_float(row["median_nodes_expanded"])
        if expanded is None or expanded <= 0:
            continue        # nothing solved in this cell, so there is no median to plot
        n = reachable_state_count(int(row["colors"]), int(row["height"]))
        coverage = to_float(row["coverage"])
        by_algorithm[row["algorithm"]].append((n, expanded, coverage))

    fig, ax = plt.subplots(figsize=(AAAI_FULL, 2.9))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.grid(True, which="major", linewidth=0.5, color=GRID, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)

    plotted = 0
    for name, label, color, marker in FIG1_SERIES:
        points = sorted(by_algorithm.get(name, []))
        if not points:
            print(f"warning: no data for {name}; omitted from figure 1", file=sys.stderr)
            continue
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        ax.plot(xs, ys, color=color, linewidth=1.4, zorder=3,
                solid_capstyle="round", label=label)
        # Filled marker where the algorithm solved the whole cell; hollow where it did
        # not. A median over survivors only is a biased estimate of that cell's cost,
        # and the hollow marker is what stops the tail of each line being read as if it
        # were comparable to the filled part.
        for x, y, coverage in points:
            full = coverage is not None and coverage >= 0.999
            ax.plot(x, y, marker=marker, markersize=4.0, zorder=4,
                    color=color,
                    markerfacecolor=color if full else SURFACE,
                    markeredgecolor=color, markeredgewidth=1.0,
                    linestyle="none")
        plotted += 1

    if not plotted:
        sys.exit("error: no series had data -- is per_cell.csv from the core sweep?")

    ax.set_xlabel("reachable configurations $N$ (log scale)")
    ax.set_ylabel("median nodes expanded")

    handles = [Line2D([], [], color=c, marker=m, markersize=4.0, linewidth=1.4,
                      label=lab)
               for _, lab, c, m in FIG1_SERIES if by_algorithm.get(_)]
    handles.append(Line2D([], [], color=INK_MUTED, marker="o", markersize=4.0,
                          markerfacecolor=SURFACE, markeredgecolor=INK_MUTED,
                          linestyle="none", label="cell not fully solved"))
    ax.legend(handles=handles, loc="center left", bbox_to_anchor=(1.01, 0.5),
              frameon=False, handlelength=1.8, labelspacing=0.5)

    for fmt in formats:
        path = os.path.join(outdir, f"fig_expansions_vs_states.{fmt}")
        fig.savefig(path)
        print(f"wrote {path}")
    plt.close(fig)


# ---------------------------------------------------------------------------
# Figure 2
# ---------------------------------------------------------------------------

def figure_coverage(cells, kills, have_logs, outdir, formats):
    """Solved / timeout / OOM per algorithm, as horizontal stacked bars."""
    solved = defaultdict(int)
    attempted = defaultdict(int)
    for row in cells:
        solved[row["algorithm"]] += int(row["instances_solved"])
        attempted[row["algorithm"]] += int(row["instances_attempted"])

    order = sorted(solved, key=lambda a: (solved[a] / max(attempted[a], 1), solved[a]))

    fig, ax = plt.subplots(figsize=(AAAI_COL, 0.20 * len(order) + 1.0))
    ax.grid(True, axis="x", linewidth=0.5, color=GRID, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)

    for i, algorithm in enumerate(order):
        killed = attempted[algorithm] - solved[algorithm]
        if have_logs:
            timeout = kills[algorithm].get("timeout", 0)
            oom = kills[algorithm].get("oom", 0)
            # Anything the logs did not classify stays visible as timeout rather than
            # vanishing, so the bar always sums to the number of runs attempted.
            timeout += max(0, killed - timeout - oom)
        else:
            timeout, oom = killed, 0

        left = 0.0
        for value, (_, _, color) in zip((solved[algorithm], timeout, oom), OUTCOME):
            if value <= 0:
                left += value
                continue
            # A 2px-equivalent surface gap between segments, drawn as an edge in the
            # page color rather than as a border around each mark.
            ax.barh(i, value, left=left, height=0.62, color=color,
                    edgecolor=SURFACE, linewidth=0.8, zorder=3)
            left += value

        ax.text(attempted[algorithm] + 8, i, f"{solved[algorithm]}",
                va="center", ha="left", fontsize=6.5, color=INK_MUTED)

    ax.set_yticks(range(len(order)))
    ax.set_yticklabels(order, fontsize=6.5)
    ax.tick_params(axis="y", length=0)
    ax.set_xlabel("runs (of 660 core instances)")
    ax.set_xlim(0, max(attempted.values(), default=660) * 1.10)

    labels = OUTCOME if have_logs else [OUTCOME[0], ("killed", "killed", SLOT[1])]
    ax.legend(handles=[Patch(facecolor=c, label=lab) for _, lab, c in labels],
              loc="lower right", frameon=False, fontsize=6.5)

    if not have_logs:
        print("warning: no sweep logs found, so timeouts and OOM kills could not be "
              "separated; figure 2 shows one undifferentiated killed segment",
              file=sys.stderr)

    for fmt in formats:
        path = os.path.join(outdir, f"fig_coverage.{fmt}")
        fig.savefig(path)
        print(f"wrote {path}")
    plt.close(fig)


# ---------------------------------------------------------------------------
# The numbers the paper quotes in prose
# ---------------------------------------------------------------------------

def report_numbers(rows, cells):
    """Print the per-algorithm figures the paper states in text.

    Two aggregations are printed side by side because they are not the same number
    and the paper should say which it means: the median pooled over every solved run,
    and the median of the per-cell medians (the paper's own protocol aggregates per
    cell first). Where a cell is unsolved it contributes to neither.
    """
    pooled_runtime = defaultdict(list)
    pooled_expanded = defaultdict(list)
    for row in rows:
        if row["solved"] not in ("1", "True", "true"):
            continue
        pooled_runtime[row["algorithm"]].append(float(row["runtime_seconds"]))
        pooled_expanded[row["algorithm"]].append(float(row["nodes_expanded"]))

    cell_runtime = defaultdict(list)
    cell_expanded = defaultdict(list)
    for row in cells:
        r = to_float(row["median_runtime_seconds"])
        e = to_float(row["median_nodes_expanded"])
        if r is not None:
            cell_runtime[row["algorithm"]].append(r)
        if e is not None:
            cell_expanded[row["algorithm"]].append(e)

    def med(values):
        return statistics.median(values) if values else float("nan")

    print()
    print("Per-algorithm medians (fill these into the paper rather than inferring):")
    print(f"{'algorithm':<20} {'runtime pooled':>14} {'runtime by cell':>16} "
          f"{'expanded pooled':>16} {'expanded by cell':>17}")
    for algorithm in sorted(pooled_runtime):
        print(f"{algorithm:<20} {med(pooled_runtime[algorithm]):>14.4f} "
              f"{med(cell_runtime[algorithm]):>16.4f} "
              f"{med(pooled_expanded[algorithm]):>16.0f} "
              f"{med(cell_expanded[algorithm]):>17.0f}")
    print()
    print("The two the paper currently infers rather than measures:")
    print(f"  greedy median runtime        = {med(pooled_runtime['greedy']):.4f} s "
          f"(pooled) / {med(cell_runtime['greedy']):.4f} s (by cell)")
    for w in ("wastar-1.005", "wastar-1.01"):
        print(f"  {w} median expanded  = {med(pooled_expanded[w]):.0f} (pooled) / "
              f"{med(cell_expanded[w]):.0f} (by cell)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", default="results/all_runs.csv")
    ap.add_argument("--cells", default="results/per_cell.csv")
    ap.add_argument("--logs", default="logs/sweep")
    # Not under results/: .gitignore excludes results/ and *.csv, so figures written
    # there could not be committed. paper/figures/ is tracked and sits next to main.tex,
    # which is also what \includegraphics resolves against.
    ap.add_argument("--outdir", default="paper/figures")
    ap.add_argument("--format", default="pdf,png",
                    help="comma-separated output formats (default pdf,png)")
    ap.add_argument("--no-numbers", action="store_true",
                    help="skip the per-algorithm number report")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    formats = [f.strip() for f in args.format.split(",") if f.strip()]

    cells = read_csv_rows(args.cells)
    rows = read_csv_rows(args.rows)
    kills, n_logs = load_kill_reasons(args.logs)

    figure_expansions(cells, args.outdir, formats)
    figure_coverage(cells, kills, n_logs > 0, args.outdir, formats)

    if not args.no_numbers:
        report_numbers(rows, cells)


if __name__ == "__main__":
    main()