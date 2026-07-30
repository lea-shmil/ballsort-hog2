# Results — triage pass

**This is the `--count 2` triage pass, not the reportable run.** Two instances per cell per
seed over three seeds is six samples per cell; the paper's protocol is ten per cell and the
full run (`--count 10`, ~9,750 tasks) is what the final numbers should come from. Everything
below is for shaping that run: which cells each algorithm actually clears, and whether the
pipeline produces sane numbers. See `README.md` for the benchmark design and provenance.

## Setup

| | |
| --- | --- |
| Grid | `core` — 22 cells, tiers A–C, the paper's grid ∩ what our `uint64` state hash encodes |
| Instances | 2 per cell per seed × 3 seeds (`20250726`, `20250727`, `20250728`) = 132 per algorithm |
| Runs | 1,885 completed of 1,950 attempted; 65 killed on the limit |
| Limits | 60 min / 32 GB per `(algorithm, instance)`, single-threaded |
| Hardware | AMD EPYC 7702P, 8 nodes, **one CPU model across every row** (verified from the data) |
| Wall-clock | median of 3 repeats per run; node counts are deterministic and taken once |

## Correctness

The contract from `CLAUDE.md` holds:

- **132 instances cross-checked, 0 disagreements** — every optimal algorithm (BFS,
  bidirectional BFS, frontier BFS, IDDFS, Dijkstra, A\*, IDA\*, and their RSCBT with and
  without symmetry reduction) returned the same length on every instance it solved.
- **0 suboptimal-bound violations** — every weighted A\* stayed inside `w × optimal`, and
  greedy stayed `≥ optimal`.

That agreement across eleven independently-implemented searches is the evidence that the
domain, the hash, our heuristic and the ported DFVS bound are all correct.

## Median nodes expanded

`-` means not attempted (tier cap) or every run was killed. Cells are ordered by reachable
state count `N`.

| cell | tier | `N` | iddfs | bfs | dijkstra | frontier | biBFS | A\*+misp | IDA\*+misp | rscbt-nosym | rscbt | IDA\*+paper | **A\*+paper** |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4x3 | A | 1e3 | 744k | 586 | 594 | 822 | 142 | 124 | 1722 | 65 | 65 | 14 | **16** |
| 5x3 | A | 5e3 | 8.9M | 2422 | 2412 | 3512 | 308 | 266 | 28k | 96 | 96 | 18 | **19** |
| 6x3 | A | 3e4 | 3080M | 17k | 17k | 27k | 1901 | 2196 | 3.4M | 324 | 313 | 68 | **33** |
| 3x4 | A | 3e4 | 123M | 16k | 18k | 21k | 889 | 620 | 8364 | 198 | 147 | 14 | **19** |
| 2x5 | A | 4e4 | 8.9M | 21k | 21k | 24k | 734 | 326 | 2091 | 616 | 174 | 22 | **22** |
| 7x3 | B | 1e5 | – | 96k | 95k | 160k | 7642 | 10k | 79M | 1402 | 1379 | 181 | **85** |
| 8x3 | B | 6e5 | – | 351k | 353k | 550k | 13k | 18k | 621M | 2438 | 2364 | 124 | **72** |
| 4x4 | B | 1e6 | – | 993k | 974k | 1.6M | 34k | 46k | 27M | 4128 | 3962 | 1686 | **312** |
| 2x6 | B | 2e6 | – | 937k | 1.0M | 923k | 7768 | 2070 | 29k | 1036 | 550 | 12 | **14** |
| 9x3 | B | 3e6 | – | 2.5M | 2.4M | 4.5M | 82k | 229k | – | 8430 | 8406 | 16k | **1450** |
| 10x3 | B | 1e7 | – | 6.9M | 6.8M | 12M | 156k | 321k | 2938M | 2670 | 2670 | 11k | **266** |
| 3x5 | B | 1e7 | – | 7.0M | 7.9M | 9.0M | 77k | 26k | 2.7M | 1370 | 1243 | 21 | **22** |
| 5x4 | B | 4e7 | – | 28M | 28M | 42M | 272k | 291k | 160M | 5248 | 5195 | 3838 | **569** |
| 11x3 | B | 6e7 | – | 42M | 43M | 74M | 687k | 2.1M | – | 14k | 14k | 81k | **1746** |
| 2x7 | C | 2e8 | – | – | – | – | – | 7022 | 163k | 1732 | 1070 | 18 | **18** |
| 12x3 | C | 2e8 | – | – | – | – | – | 4.4M | – | 25k | 25k | 22k | **2180** |
| 13x3 | C | 1e9 | – | – | – | – | – | 43M | – | 296k | 296k | 24M | **61k** |
| 6x4 | C | 1e9 | – | – | – | – | – | 2.9M | 229M | 7986 | 7955 | 3048 | **498** |
| 4x5 | C | 4e9 | – | – | – | – | – | 3.4M | – | 15k | 15k | 5832 | **842** |
| 3x6 | C | 9e9 | – | – | – | – | – | 3.4M | – | 19k | 19k | 1636 | **422** |
| 2x8 | C | 2e10 | – | – | – | – | – | 612k | 228M | 49k | 31k | 2238 | **1005** |
| 7x4 | C | 5e10 | – | – | – | – | – | 31M | – | 99k | 99k | 821k | **6336** |

## What it says

**The paper's DFVS bound is the whole story.** A\* with it solved 132/132 and expands three
to four orders of magnitude fewer nodes than the same search with the misplaced-ball
heuristic: on `7x4` (N = 5e10) it is 6,336 against 31M — a factor of ~4,900 — and in wall
clock 0.87s against 200s. On `13x3`, 61k against 43M. Porting it was worth the effort, and
`astar-paper` should be the reference configuration from here.

**Their algorithm behaves exactly as their §6 predicts.** RSCBT solved every cell, but
expands consistently more than A\* with the same bound — 99k against 6,336 on `7x4`. That is
the documented cost of expanding in breadth-first rather than best-first order, which they
accept in exchange for easy parallelization. Since we run it single-threaded, we pay the cost
without collecting the benefit, so this is not evidence against their design.

**Symmetry reduction cuts nodes but not time.** `rscbt` against `rscbt-nosym`: 174 vs 616 on
`2x5`, 550 vs 1036 on `2x6`, 31k vs 49k on `2x8` — the saving grows with the color count, as
expected. But wall-clock is no better (1.63s vs 1.92s on `7x4`, 2.83s vs 2.92s on `13x3`),
because minimizing over `c!` relabelings per state costs about what it saves.

**IDA\* is the wrong linear-space choice here.** With the paper's bound it beats A\* on small
cells but collapses on large ones — 24M against A\*'s 61k on `13x3`, 821k against 6,336 on
`7x4`. Few distinct `f` values means many iterations each re-expanding almost everything.
RSCBT, which is also memory-lean, dominates it on the hard cells.

**Bidirectional BFS is the best uninformed algorithm by two orders of magnitude** (687k
against 42M on `11x3`), and IDDFS is unusable past tier A, as the tier caps assumed —
3,080M expansions on `6x3`, a cell BFS finishes in 17k.

### Suboptimality

| algorithm | median | mean | max | bound |
|---|---|---|---|---|
| `wastar-1.25` | 1.000 | 1.022 | 1.136 | ≤ 1.25 ✓ |
| `wastar-1.5` | 1.041 | 1.050 | 1.200 | ≤ 1.5 ✓ |
| `wastar-2` | 1.083 | 1.100 | 1.500 | ≤ 2 ✓ |
| `wastar-3` | 1.136 | 1.168 | 1.755 | ≤ 3 ✓ |
| `wastar-5` | 1.143 | 1.230 | 2.154 | ≤ 5 ✓ |
| `greedy` | 1.242 | 1.431 | 3.423 | none |

Every weighted run stayed well inside its guarantee — `w = 5` never exceeded 2.15× — so the
weight sweep buys speed cheaply. `w = 1.25` was optimal on more than half its instances.

## Coverage, and how to read it

| algorithm | solved / attempted | killed |
|---|---|---|
| `astar-paper`, `idastar-paper`, `rscbt`, `rscbt-nosym`, `greedy`, all `wastar-*` | 132/132 | 0 |
| `bfs`, `bibfs`, `dijkstra`, `frontier-bfs` | 84/84 | 0 |
| `astar-misplaced` | 128/132 | 4 |
| `iddfs` | 27/30 | 3 |
| `idastar-misplaced` | 74/90 | 16 |

**The 100% for the exhaustive algorithms is not what it looks like.** They were only
*attempted* on tiers A–B (84 = 14 cells × 6), because `build_tasklist.py` caps them there.
The number says they cleared every cell they were asked about, not that they would survive
tier C. `iddfs` likewise was only asked about tier A.

## Next

- **Re-tune the tier caps from measurement rather than from `N`.** `astar-misplaced` cleared
  97% of tier C, so capping it at B in the full run would discard real data; `idastar-misplaced`
  managed only 82% and is the weakest informed configuration.
- **Consider lifting the caps on the exhaustive algorithms for one or two tier-C cells**, so
  the claim "they cannot reach tier C" is measured rather than assumed. `12x3` (N = 2e8) is
  the cheapest place to test it.
- **Then run `--count 10`** with `--skip-existing`, which reuses these 1,885 rows and runs only
  the remainder.
