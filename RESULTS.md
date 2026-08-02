# Results: the best-first family on the ball-sort domain

Full sweep, 2026-08-02. **11,220 runs attempted, 9,828 completed, 1,392 killed.**

17 algorithms x 22 cells x 10 instances x 3 seeds. Every run single-process on an
`ise-cpu128-*` node (AMD EPYC 7702P, one CPU model across all 12 nodes, verified per row),
32 GB and 3600 s per run, wall-clock the median of 3 repeats. See `README.md` for the
benchmark design and provenance.

Reproduce with:

```
slurm/build_tasklist.py --grid core --seeds 20250726,20250727,20250728
bash slurm/submit_sweep.sh
slurm/aggregate_results.py
```

> **Memory caveat.** This dataset was collected with a flat 32 GB per run. The committed
> configuration now sizes memory per algorithm (60 GB for the closed-list searches), so a
> fresh run will *not* reproduce the coverage numbers below exactly -- it should do slightly
> better on `12x3` and `13x3`. Everything else, including all node counts and solution
> lengths, is hardware- and limit-independent and reproduces exactly. See
> "What more memory would buy" below.

## Correctness contract

The point of running seven optimal algorithms on the same instances is that they must agree.

| check | result |
| --- | --- |
| instances cross-checked across optimal algorithms | 660 |
| disagreements on optimal solution length | **0** |
| weighted-A\* runs violating their `w` bound | **0** |
| distinct CPU models in the timing data | 1 |

`rscbt` and `rscbt-par` also expand **byte-identical node counts on all 660 instances**, which
is the determinism property their contiguous-slice merge was written to guarantee.

## Coverage: who finishes at all

The most informative single table. Denominator is 660 attempts (22 cells x 10 x 3 seeds).

| algorithm | solved | coverage | median runtime (s) |
| --- | ---: | ---: | ---: |
| `astar-paper` (A\* + DFVS bound) | 660 | **100.0%** | 0.0036 |
| `rscbt`, `rscbt-nosym`, `rscbt-par` | 660 | **100.0%** | 0.043 / 0.031 / 0.041 |
| `wastar-1.005 … 1.1`, `greedy` | 660 | **100.0%** | 0.0012 – 0.0047 |
| `bibfs` | 658 | 99.7% | 0.120 |
| `idastar-paper` | 655 | 99.2% | 0.0050 |
| `astar-misplaced` | 636 | 96.4% | 0.098 |
| `bfs` | 480 | 72.7% | 1.548 |
| `frontier-bfs` | 467 | 70.8% | 0.998 |
| `dijkstra` | 459 | 69.5% | 1.783 |
| `idastar-misplaced` | 375 | 56.8% | 0.195 |
| `iddfs` | 158 | **23.9%** | 2.077 |

## The heuristic is the whole story

On the 636 instances both A\* variants solved:

| heuristic | median nodes expanded |
| --- | ---: |
| misplaced balls (ours) | 61,934 |
| DFVS bound (the paper's) | **100** |

**619x fewer expansions**, and it moves A\* from 96.4% coverage to 100%. The same swap takes
IDA\* from 56.8% to 99.2%. Nothing else in this study comes close to that effect size.

## The paper's own algorithm

`rscbt` solves every instance, but as a *search* it is not competitive with best-first on the
same bound:

| | median nodes expanded |
| --- | ---: |
| `astar-paper` | 116 |
| `rscbt` | 2,687 |

That is expected rather than a defect: RSCBT is breadth-first layer-by-layer under an
ascending `mu` filter, so it does not order by `f` and re-derives work each round. Its value
is that it is the paper's published method and it parallelizes cleanly.

- **Symmetry reduction** (`rscbt` vs `rscbt-nosym`): 2,687 vs 3,072 median expansions, a
  1.14x cut -- real but modest at these cell sizes.
- **Parallel speedup** (`rscbt-par`, 16 threads): 0.99x median across all 660 instances,
  because most finish in milliseconds and thread setup dominates. Restricted to the 121
  instances with more than 1 s of serial work: **2.82x median, 7.67x max.**

## Suboptimal search

Weighted A\* on this domain barely trades anything, in either direction:

| algorithm | mean ratio | max ratio | optimal on |
| --- | ---: | ---: | ---: |
| `wastar-1.005` | 1.0000 | 1.000 | 660/660 (100%) |
| `wastar-1.01` | 1.0000 | 1.000 | 660/660 (100%) |
| `wastar-1.05` | 1.0000 | 1.000 | 660/660 (100%) |
| `wastar-1.1` | 1.0017 | 1.050 | 627/660 (95%) |
| `greedy` | 1.4160 | 3.423 | 149/660 (23%) |

Every weighted run stayed inside its `w` bound. But the median expansions tell the real story:
116 for A\*, 116 at w=1.05, 92 at w=1.1. **Weighting buys essentially nothing here**, because
the DFVS bound is already tight enough that A\* expands about 100 nodes. Greedy does cut
expansions 3.74x, at a mean 42% longer solution.

## Why runs died: timeout vs out of memory

The 1,392 killed runs were re-run to separate the two causes, which an earlier
`timeout --signal=KILL` had made indistinguishable (both exit 137). The split is
**860 timeouts / 532 out-of-memory**, and it falls cleanly along algorithm class:

| algorithm | timeout | out of memory |
| --- | ---: | ---: |
| `iddfs` | 502 | 0 |
| `idastar-misplaced` | 285 | 0 |
| `idastar-paper` | 5 | 0 |
| `frontier-bfs` | 63 | 130 |
| `dijkstra` | 3 | 198 |
| `bfs` | 2 | 178 |
| `astar-misplaced` | 0 | 24 |
| `bibfs` | 0 | 2 |

The linear-space searches recorded **zero** OOMs -- exactly as their space complexity predicts --
while the closed-list searches account for every one. This is the cross-check that the domain's
memory behaviour is what the theory says.

By cell, OOM appears only on the large-state-count cells (`12x3`, `13x3`, `2x8`, `3x6`, `4x5`,
`6x4`, `7x4`). Small cells such as `10x3` and `11x3` produced timeouts only.

## What more memory would buy

Measured, not projected. Runs that completed peaked at **31.5 GB against the 32 GB cap**, so
the boundary was genuinely binding:

- **`12x3` is the clear win.** `dijkstra` peaked at 31.4 GB on instances that finished and
  OOMed on 20 others -- it sits exactly on the boundary and should clear it at 60 GB.
- **`13x3` `astar-misplaced`** peaked at 19.8 GB with 8 OOMs; those should clear too.
- **`13x3` `bfs`/`dijkstra`** (60 OOMs) need roughly 164 GB and will not.
- Beyond that: `6x4` ~216 GB, `4x5` ~660 GB, `3x6` ~1.4 TB, `2x8` ~3.7 TB, `7x4` ~7.2 TB.

So a 60 GB re-run would recover on the order of 28 runs, not hundreds. The rest is not a
misconfiguration -- exhaustive search on those cells is out of reach on this cluster, which is
the same thing the paper reports as empty boxes in its Figure 6. The 60 GB ceiling itself is
set by contention, not policy: the nodes have 514 GB but run mostly allocated to other users,
so larger requests are accepted and then pend on Resources indefinitely.

## Reading the outputs

| file | contents |
| --- | --- |
| `results/all_runs.csv` | one row per (algorithm, instance, seed), 18 columns |
| `results/per_cell.csv` | per-cell medians and coverage |
| `results/contract.txt` | the cross-check report above |
| `logs/sweep/*.out` | per-run `KILLED reason=…` lines with elapsed time |
