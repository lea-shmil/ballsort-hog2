# ballsort-hog2

Ball-sort ("colored balls in colored tubes") domain implemented in
[HOG2](https://github.com/MovingAILab/hog2), with the best-first search family run over it for
Althaus et al., SoCS 2025. See `CLAUDE.md` for the full project brief.

## Provenance and attribution

The problem definition and the entire benchmark design in this repo come from:

> Althaus, E.; Blumenstock, M.; Rassau, N.; Schuhknecht, F. M.; and Zimdars, A. Q. 2025.
> Sorting colored balls in colored tubes. In *Proceedings of the 18th International Symposium on
> Combinatorial Search (SoCS 2025)*, 11–19. Glasgow, United Kingdom: AAAI Press.
> [Paper](https://ojs.aaai.org/index.php/SOCS/article/view/35971) ·
> [Code and datasets](https://gitlab.rlp.net/rassau/sorting-colored-balls)

**What we take from them.** All of the following originates with the authors and is reimplemented
here in C++, with the source noted at each use site:

| Borrowed | Where it comes from | Where it lives here |
| --- | --- | --- |
| Problem definition (uniform height profile, empty reserve, full colored tubes) | Paper, Definitions 1–2 ("SCBT" / "RSCBT") | `environments/BallSort.h` |
| Instance-generation procedure (uniform shuffle of the color multiset, sliced into tubes) | Paper §7; exact procedure confirmed against their `.vscode/generate_examples.py` | `RandomColorSequence` |
| Experimental protocol (ten instances per `(h, c+1)` cell, report the median) | Paper §7, quoted verbatim in `instance_gen.h` | `--count` default of 10 |
| Instance file format (one line per tube, tube id first, reserve last as tube `0`) | Their published `resources/paper_inputs/*.in` | `WritePaperInstance` / `ReadPaperInstance` |
| Cell naming `{h}x{numTubes}`, file naming `random_generated_{h}x{T}_{i}.in`, directory layout | Their repository layout and `heat_map_generate_input.sh` | `GridCell::Name`, grid mode |
| The 53-cell parameter grid | Transcribed from their `resources/paper_inputs/` listing | `PaperGridCells()` |
| The "simple lower bound" on the number of moves | Paper §6, Definition 3 and Lemma 4.4, illustrated in their Figure 4 | `SimpleLowerBound`, `BasicLowerBound` |
| The DFVS-based lower bound, graph construction and formula | Their `src/dfvs_interface.cpp` and `src/tube_rack.cpp`; paper §5 | `dfvs_bound.h` |
| The search algorithm (breadth-first branch and bound, `μ` filter, in/out-of-bound sets) | Paper §6, Algorithm 1 | `rscbt.h` |
| The color-permutation symmetry reduction | Paper §6 | `RSCBTRepresentative` |
| The four reported metrics, and the median-over-instances protocol | Paper §7, Figure 6 | `run_experiment`, `aggregate_results.py` |
| Reachable-configuration count `N = (h+c)!·(hc)!/(c!·(h!)^(c+1))` | Paper §7 | `ReachableStateCount` |

**What is ours.** The HOG2 domain implementation and state hash, the seeded and extensible instance
draws, the read-back verification and manifest, the difficulty tiering, `--min-lb`, the strict
reader, the exact minimum-DFVS solver in `dfvs_exact.h` (replacing the PACE solver they vendor),
the experiment runner, the SLURM sweep and its aggregation, and every algorithm in the comparison
other than `rscbt`.

**No code or data of theirs is vendored or redistributed here.** Their repository is public but
carries no license file — the only `LICENSE.txt` in it belongs to a third-party DFVS solver they
vendor — so there is no grant to copy it. Everything above was reimplemented from the paper's
description and from inspecting their published artifacts, which is also why our instance files are
generated from our own seed rather than copied from their `paper_inputs/`. Because we adopted their
file format, their published `.in` files can still be read directly (`generate_instances --read`) if
we ever want to run against their exact instances; that would mean fetching them from their
repository, under whatever terms they set, not checking them in here.

Note that their solver is a 16-thread, disk-backed BFS with a DFVS-based lower bound, so their
reported reach is not a like-for-like baseline for the single-threaded textbook algorithms compared
here — see the difficulty tiers below.

## Cloning

HOG2 is vendored as a **git submodule** at `./hog2` (branch `PDB-refactor`), so clone recursively:

```sh
git clone --recurse-submodules <repo-url>
```

If you already cloned without it:

```sh
git submodule update --init --recursive
```

The submodule is a large checkout (~700 MB) and takes several minutes to fetch.

## Build

The build needs the `ballsort` conda env active — it supplies the C++ toolchain. This cluster has
no system `g++` or `cmake`, and no gcc/cmake environment modules, so conda is where the compiler
comes from.

Create the env once:

```sh
conda env create -f environment.yml
```

Then, for every build (and inside every SLURM job script):

```sh
conda activate ballsort
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/ballsort
```

Builds are **Release** (`-O3`) — this is a search-timing study, so never benchmark a debug build.

## Layout

| Path | Contents |
| --- | --- |
| `hog2/` | HOG2 submodule (upstream, unmodified) |
| `include/hog2_prelude.h` | Standard headers HOG2 assumes; include before any HOG2 header |
| `include/frontier_bfs.h` | Our own frontier BFS (hog2's is unusable — see below) |
| `include/iddfs.h` | Our own IDDFS/DFID (hog2's is unusable — see below) |
| `include/instance_gen.h` | Instance generation, the parameter grid, the paper's file format and simple bound |
| `include/dfvs_exact.h` | Our exact minimum-DFVS branch and bound (replaces their vendored PACE solver) |
| `include/dfvs_bound.h` | The paper's DFVS lower bound, as a HOG2 `Heuristic` |
| `include/rscbt.h` | The paper's own algorithm (their Algorithm 1) |
| `environments/BallSort.h` | The domain: state, moves, goal test, hash, heuristic |
| `src/main.cpp` | `ballsort` — the domain + algorithm-family smoke test |
| `src/test_dfvs.cpp` | `dfvs_check` — validation for the ported bound and the solver under it |
| `src/generate_instances.cpp` | `generate_instances` — writes reproducible instance files |
| `src/run_experiment.cpp` | `run_experiment` — one algorithm on one instance, one CSV row |
| `slurm/build_and_generate.sbatch` | Build + generate the instance set + run the smoke test, on a compute node |
| `slurm/build_tasklist.py` | Generate the multi-seed instance sets and the sweep's task list |
| `slurm/experiment_sweep.sbatch` | The sweep itself: one array task per (algorithm, instance) |
| `slurm/submit_sweep.sh` | Chunked array submission (MaxArraySize here is 1001) |
| `slurm/aggregate_results.py` | Pool rows into per-cell medians; check the correctness contract |

The domain and the full best-first search family from `CLAUDE.md` are wired up, and the instance set
for the experiments is generated and checked in as reproducible-from-seed (see below). There is not
yet an experiment driver — that is next.

`./build/ballsort` runs a self-check and exits. It verifies the domain invariants (action legality,
`ApplyAction`/`UndoAction` symmetry, ball conservation, and that the state hash is injective and
invertible over the whole reachable set); confirms that every optimal algorithm — BFS, bidirectional
BFS, frontier BFS, IDDFS/DFID, Dijkstra, A\*, IDA\* — returns the same optimal length on several
random instances, and that weighted A\* and greedy best-first respect their own (weaker) bounds; and
checks the instance machinery, namely that the paper's simple lower bound never exceeds the optimal
length BFS reports, that it dominates `BallSort::HCost`, and that instance files round-trip through
the writer/reader pair `generate_instances` uses.

Only five HOG2 directories are on the include path — `search`, `generic`, `algorithms`, `utils`,
`simulation`. The build is headless: no `gui/`, no SFML, no OpenGL.

Note that the algorithm headers do not live where the directory names suggest: `BFS.h`,
`FrontierBFS.h`, `UnitCostBidirectionalBFS.h`, `DFID.h`, `IDAStar.h` and `TemplateAStar.h` are all in
`hog2/generic/`, while `hog2/search/` holds `SearchEnvironment.h` and the PDB machinery.

**Two of hog2's algorithm headers are unusable as shipped on this branch (`PDB-refactor`), and we
don't patch vendored code (see "Working on this repo" below), so we reimplemented them instead:**

- `hog2/generic/DFID.h` has its goal test commented out in both `GetPath` overloads, so a search
  never succeeds and just raises its depth bound forever. `IDAStar` + a zero heuristic looked like a
  clean substitute (IDDFS is, in principle, exactly IDA\* with h=0) — except `IDAStar`'s own success
  check can't tell "goal found" apart from "root heuristic is exactly 0 and the bound was exhausted,"
  which is every call when h≡0. Two independent bugs, so `include/iddfs.h` is our own.
- `hog2/generic/FrontierBFS.h` never calls `GoalTest` and its `GetPath` never populates the output
  path — it silently runs to full exhaustion of the reachable state space instead. `include/frontier_bfs.h`
  reimplements it with proper early termination on the goal.

IDDFS/DFID is also only run against the small `BallSort(3,2)` instance in the smoke test, not
`BallSort(3,3)`. With no heuristic and a high branching factor (no color matching, so up to
`numTubes*(numTubes-1)` moves per state), plain iterative deepening on `(3,3)` cost 161M node
expansions and 52s wall-clock for a single instance in testing — real timing work that belongs on a
compute node via `sbatch`/`srun`, not this login-node smoke test.

## The instance set

The benchmark design follows Althaus et al. rather than being invented here, so that our
search-algorithm comparison sits on the same instances their solver was measured on. See
[Provenance and attribution](#provenance-and-attribution) for what specifically is theirs — the
generation procedure, protocol, file format, naming, grid and lower bound all are.

**What the paper does.** Their Definition 2 ("RSCBT") is exactly our domain: every tube, reserve
included, has the same height `h`; the start configuration has the reserve empty and all `c` colored
tubes full. Instances are "randomly generated initial tube-rack configurations" — a uniform shuffle
of the multiset of `h` copies each of colors `1..c`, sliced into the colored tubes in order, with no
solvability check and no difficulty filter. Their generator
(`.vscode/generate_examples.py` in [their repository](https://gitlab.rlp.net/rassau/sorting-colored-balls))
is a bare `random.shuffle` of that multiset, which is all the generation there is. Section 7: *"For
each parameter configuration (h, c+1) and metric of interest, we perform ten runs on ten different
randomly generated initial tube-rack configurations and report the median."*

**Cell naming.** The paper indexes parameter cells as `{h}x{numTubes}` with the reserve counted, so
`numTubes = c+1`. Cell `4x8` is height 4 over 8 tubes, i.e. 7 colors — the headline cell of their
abstract ("instances with 7 coloured tubes of height 4"). Their published benchmark set is
`resources/paper_inputs/{h}x{T}/random_generated_{h}x{T}_{i}.in` for `i` in `0..9`, and
`PaperGridCells()` in `instance_gen.h` is that grid transcribed: 53 cells, 530 instances.

### Generating

```sh
mkdir -p logs && sbatch slurm/build_and_generate.sbatch     # builds, generates, verifies
```

or directly, if a build is already in place:

```sh
./build/generate_instances --grid paper --outdir instances    # all 53 cells, 10 each
./build/generate_instances --grid core  --outdir instances    # just the 22 comparison cells
./build/generate_instances --read instances/4x8/random_generated_4x8_0.in
```

`--count` defaults to 10 (the paper's protocol) and `--seed` to a fixed default, so a bare invocation
is reproducible. `--grid` picks the cell set: `paper` (all 53), `runnable` (the 29 our state hash can
encode — see below), or `core` (the 22 intended for the comparison). Single-cell mode is still there
for one-offs — `--height H` with either `--colors C` or `--tubes T`, plus `--format flat` for the
one-instance-per-line format this tool used to write. `--help` covers the rest.

Each instance is seeded from `(seed, h, numTubes, index)` rather than from one running stream. Two
consequences worth relying on: cells are independent, so a job array can regenerate one cell on one
node; and the set is *extensible*, because instance `i` is a function of `i` alone. Raising `--count`
from 10 to 30 leaves instances `0..9` byte-identical, which keeps them the subset directly comparable
to the paper's ten. Picking a smaller `--grid` likewise does not change any file — `--grid core` and
`--grid paper` write identical bytes for every cell they share.

Generated files stay gitignored (`instances/`): they are fully reproducible from the seed.

**File format** is the paper's, so their published `.in` files are valid input here unchanged. One
line per tube — the tube's own id, then `h` ball colors — with the colored tubes first and the
reserve last as tube `0` with `h` zeros:

```
1 5 2 3 3
2 7 2 1 6
...
7 2 1 7 4
0 0 0 0 0
```

We read and write the `h` colors bottom-to-top, matching `BallSortState`. Their generator writes a
shuffled slice straight out without committing to a direction, so for uniformly random instances the
two readings are the same distribution; ingesting one of their files reads it as the vertical mirror
of what they saw, which is an equally valid instance of the same cell.

Alongside the instances, `instances/manifest.csv` has one row per instance: cell, `h`, tubes, colors,
balls, reachable state count, whether the state hash fits a `uint64_t`, difficulty tier, and two
per-instance difficulty tags — the paper's simple lower bound and the number of balls already in
final position.

### Difficulty: which cells, and why

"Not too simple, not too complicated" is set by *which cells* we run, not by filtering instances
within a cell — filtering would bias the distribution and break comparability with the paper's
medians. The lever is the grid.

Cells are tiered on the exact reachable state count, the paper's
`N = (h+c)!·(hc)! / (c!·(h!)^(c+1))`, because that is what BFS, bidirectional BFS, frontier BFS and
Dijkstra all pay in full:

| Tier | `N` | What survives there | Cells (`{h}x{T}`) |
| --- | --- | --- | --- |
| trivial | `< 1e3` | everything, instantly | `2x3` `3x3` `2x4` |
| **A** | `1e3 – 1e5` | IDDFS/DFID still finishes | `4x3` `5x3` `6x3` `3x4` `2x5` |
| **B** | `1e5 – 1e8` | the memory-bounded optimal set (BFS, biBFS, frontier BFS, Dijkstra, A\*) | `7x3` `8x3` `4x4` `2x6` `9x3` `10x3` `3x5` `5x4` `11x3` |
| **C** | `1e8 – 1e11` | only A\*, IDA\*, weighted A\*, greedy best-first | `2x7` `12x3` `13x3` `6x4` `4x5` `3x6` `2x8` `7x4` |
| D | `> 1e11` | nothing, with the current heuristic | 27 cells, `5x5` through `3x12` |

The **core set** is tiers A–C minus what the state encoding rules out: **22 cells, 220 instances**.
Tier A is in it to give IDDFS/DFID something it can actually finish, and tier C to give the
heuristic-guided algorithms something the uninformed ones cannot — a comparison where every algorithm
solves everything would not separate them. Cells outside the core set are still generated (`--grid
paper`), because a sweep that reports timeouts on tier D is a result, not a gap.

The tier cuts are a-priori, from `N`; the sweep is what turns them into measured reach. The one
measurement already in hand sets tier A's ceiling: on `3x4` (`BallSort(3,3)`, `N = 3.4e4`) plain
iterative deepening cost 161M expansions and 52s for a single instance, because it has no closed list
and re-expands. That is why the IDDFS ceiling is four orders of magnitude below everything else's.

No cell is degenerate: over all 530 instances the smallest simple lower bound is 2 (in `2x3`, the
4-ball cell), and at most 8 balls are ever already in final position.

**One deviation from the paper's protocol**, and it is small: `--min-lb` defaults to 1, which redraws
an arrangement that is *already* the goal. That can only happen in the tiniest cells — with `h=2` and
two colors, one shuffle in six is already solved — and an instance with optimal length 0 measures
nothing. `--min-lb 0` gives their unfiltered shuffle exactly.

### The `uint64_t` state hash caps the grid below the paper's

`BallSort<c,h>`'s perfect hash packs the state as a `(c+1)*h`-digit number in base `c+1`, so it needs
`(c+1)·h·log2(c+1)` bits and `BallSort.h` static-asserts that this fits a `uint64_t`. That admits
**29 of the paper's 53 cells**. The maximum height per color count is:

| colors `c` | 2 | 3 | 4 | 5 | 6 | 7 | 8 | ≥9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| max `h` | 13 | 7 | 5 | 4 | 3 | 2 | 2 | 1 |

Two consequences to be aware of:

- **The paper's headline cell `4x8` (7 colors, height 4) cannot be instantiated at all.** It needs
  `8^32`, which is `2^96`. It is tier D for us regardless, so this is not blocking today, but it does
  mean we cannot state a result on the specific cell their abstract advertises.
- **`14x3` is the one cell we lose that we would otherwise want.** It is tier C, `N = 4.8e9`, and
  needs `3^42` ≈ `1.1e20` — just over `2^64`. It is excluded from the core set for encoding reasons
  only, not difficulty.

Both would be fixed by ranking states densely over the reachable set instead of over all digit
strings (the current range is sparse — digit strings with a gap in a tube are unreachable). That is a
change to `GetStateHash`/`GetStateFromHash`/`GetMaxHash`, not to the instance set, and is not done.

### The paper's simple lower bound is stronger than our current heuristic

`instance_gen.h` implements the paper's *simple lower bound* (their Section 6, Figure 4), used here
as a per-instance difficulty tag. From their Definition 3, a ball of color `i` is in *final position*
if it is in tube `i` and every ball below it is also color `i`; then a ball in final position never
moves (0), a ball of color `i` in tube `i` but *not* in final position must come out and go back (2),
and any other ball moves at least once (1). Summing is admissible because each move relocates exactly
one ball.

The smoke test checks that bound against BFS ground truth on every instance it runs, and checks that
it dominates `BallSort::HCost`. It does, sometimes by a lot — on `3x4` instance 3, optimal is 13, the
simple bound is 11, and `HCost` is 5. `HCost` scores a ball sitting in its own tube on top of a
foreign ball as 0 when the true cost is 2. So the paper's simple bound is the heuristic A\*/IDA\*
should switch to, which resolves the open item in `CLAUDE.md`; it needs generalizing from start
states to arbitrary states first (it is currently written against the flat start sequence), plus a
consistency check like the one `CheckDomainInvariants` already runs.

`generate_instances` is deliberately independent of HOG2 and of `BallSort.h` — see the comment at the
top of `generate_instances.cpp`. Generating an instance is pure combinatorics, so it isn't linked
against the search machinery. It verifies each instance's color counts before writing and reads every
file back after writing, because a silently corrupt instance file would be expensive to discover
after a cluster run.

## Working on this repo

**`git pull` does not update the submodule.** This is the easiest way to silently build against the
wrong HOG2 revision: a plain pull leaves `hog2/` at whatever commit you first checked out, even when
someone else has moved the pin. After every pull:

```sh
git pull
git submodule update --init --recursive
```

**Never edit anything under `hog2/`.** It is an unmodified upstream checkout, and that is worth
preserving: any edit there dirties the submodule and can bump the recorded pin, which silently
changes what everyone else compiles. If a HOG2 header fails to compile because it uses a `std::` type
it never included — gcc 15's libstdc++ no longer pulls several of them in transitively — add the
missing header to `include/hog2_prelude.h` instead. That is precisely what that file is for. Check
`git status` shows `hog2` clean before committing.

## The algorithm comparison

### Algorithms

| Name | What it is |
| --- | --- |
| `bfs` `bibfs` `frontier-bfs` `iddfs` `dijkstra` | the zero-heuristic end of the family |
| `astar-misplaced` `idastar-misplaced` | with `BallSort::HCost`, the misplaced-ball count |
| `astar-paper` `idastar-paper` | with the paper's DFVS bound |
| `wastar-1.25` … `wastar-5`, `greedy` | the weight sweep and its `w = ∞` end |
| `rscbt` `rscbt-nosym` | **the paper's own algorithm**, with and without their symmetry reduction |

`rscbt` is their Algorithm 1: breadth-first (their §6 is explicit that it is *not* best-first
— breadth-first order is easier to parallelize at the cost of enumerating more
configurations), holding only levels `i-1`, `i`, `i+1`, with every generated configuration
filtered against an upper bound `μ` by an admissible lower bound, and the color-permutation
symmetry reduction collapsing each equivalence class to one representative. Algorithm 1 is a
decision procedure, so `RSCBTSolve` iterates `μ` upward from the root bound and the first
success is optimal. Two deliberate departures: **single-threaded** (they use 16 threads;
`CLAUDE.md` requires one thread per task, and timing a 16-thread algorithm against
single-threaded textbook ones would not compare anything) and **in-memory** (their disk
backing exists for cells past what our state hash can encode anyway).

### The paper's DFVS lower bound

Their strongest bound, and it is now the best heuristic we have. `LOWER-BOUND(S)` is
`basic_lower_bound(S) + dfvs_lower_bound(S)`:

- the **basic** term is the per-ball 0/1/2 count — the same rule as `SimpleLowerBound`;
- the **DFVS** term is the size of a minimum directed feedback vertex set of a graph with
  **one vertex per tube slot**, minus the DFVS members that are home-tube balls out of final
  position.

That per-slot graph is worth flagging, because it is *not* what the paper's exposition
describes. Their §5 motivates the bound as Feedback Arc Set on a graph with one vertex per
*color* (Construction 6 / Proposition 7), then reduces FAS to DFVS. Their code
(`src/dfvs_interface.cpp`) builds neither of those — it builds the per-slot graph and solves
DFVS on it directly, and that is what produced their published numbers, so it is what
`dfvs_bound.h` implements. The self-loops in that construction are deliberate: their code
carries a comment recording that removing them weakened the bound on cell `6x4` (26 instead
of 27).

They call the PACE 2022 exact-track solver "rubengoetz", vendored into their unlicensed
repository. We do not vendor it — `include/dfvs_exact.h` is our own exact branch and bound
(reduction rules, SCC decomposition, branching on a shortest cycle, bounded below by a
disjoint-cycle packing). A minimum is a minimum, so the bound *values* are identical to
theirs; only the runtime differs.

`./build/dfvs_check` validates all of it, and its results are the reason to switch the
heuristic over:

- the solver matches brute force on 648 random graphs plus the degenerate shapes;
- the bound is admissible on every instance checked against BFS ground truth;
- it **strictly dominates** the simple bound on 20/20 instances, as their §5 claims — mean
  11.40 against an optimal of 11.70 on cell `3x4`, nearly tight;
- it is **consistent** (largest `|h(n)-h(n')|` is 1 over all 33,600 states of `3x4`), so A\*
  needs no node reopening;
- A\* with it returns the optimal length everywhere and expands **42 nodes against 961** with
  `BallSort::HCost`.

One measured trade-off worth recording: their symmetry reduction cuts expansions (2397 vs
3583 on the 5-color cell `2x6`) but is *slower* in wall-clock (0.067s vs 0.020s), because
minimizing over `c!` relabelings per state costs more than the nodes it saves. Hence both
`rscbt` and `rscbt-nosym`.

### Metrics

Per `(algorithm, instance)`, `run_experiment` emits one CSV row:

| The paper's four (their Figure 6) | Ours on top |
| --- | --- |
| `runtime_seconds` | `nodes_expanded` |
| `peak_rss_kb` | `nodes_generated` |
| `max_elements_in_memory` | `solved` (coverage / timeouts) |
| `solution_length` (their "needed moves") | suboptimality ratio, `iterations`, `root_lower_bound` |

Node expansions is the metric the paper does not report — their algorithm is a layer-by-layer
BFS, so frontier size is their space metric — and it is the one that actually separates the
algorithms, being machine-independent.

### Seeds, and why they vary instances rather than runs

**Every algorithm here is deterministic**: the same instance gives byte-identical node counts
on every run, so averaging repeated runs of a fixed instance is a no-op. Repetition therefore
varies the *instances*. Each seed generates its own set of 10 instances per cell (the paper's
protocol) under `instances/seed-<S>/`, and aggregation pools them — three seeds means 30
samples per cell from the same random-instance distribution, reported as the median, as their
§7 does. Wall-clock is the one genuinely noisy quantity, so *that* is repeated inside each
run (`--timing-repeats`, default 3, median kept — median rather than mean because scheduler
noise is one-sided).

Because instance `i` is seeded from `(seed, height, tubes, i)`, seeds are independent and the
first seed's instances `0..9` remain the subset directly comparable to the paper's ten.

### Running the sweep

```sh
slurm/build_tasklist.py --grid core --seeds 20250726,20250727,20250728   # generate + task list
bash slurm/submit_sweep.sh                                              # chunked job arrays
slurm/aggregate_results.py                                              # when it finishes
```

**The sweep is pinned to one machine type.** `experiment_sweep.sbatch` carries
`--partition=cpu --constraint=cpu128`, which restricts it to the twelve `ise-cpu128-*` nodes
— all AMD EPYC 7702P at the same max clock, verified rather than assumed. This is not
housekeeping: runtime is one of the four metrics, and the `cpu` partition also holds
`cs-cpu-*` boxes and the `ise-cpu256-*` machines, while the login shell itself sits on an
Intel Xeon E5-2680 v2. An unpinned array would spread tasks across CPU generations six years
apart and the wall-clock column would be measuring hardware rather than algorithms. Node
counts and solution lengths are hardware-independent and would survive; timings would not.

Belt and braces: every row also records `node` and `cpu_model`, and
`aggregate_results.py` warns if the rows span more than one CPU model. So the pinning is
checkable from the data rather than trusted.

One array task is one `(algorithm, instance)` pair at **60 min / 32 GB, single-threaded**.
One pair per task is the design: tasks are independent, a task over either limit is killed by
SLURM without touching the rest, and a killed task is a *result* — a timeout, which is what
the paper's empty Figure 6 boxes are — rather than a lost run. Rows land one file per task in
`results/rows/`, because hundreds of concurrent tasks appending to one CSV would interleave.

By default the task list skips `(algorithm, cell)` pairs whose outcome is already known from
the tiers — `iddfs` above tier A, the exhaustive algorithms above tier B — since a 60-minute
timeout is expensive to buy thousands of times over. Every skip is written into the task list
as a comment, so "not attempted" stays distinguishable from missing data; `--all` attempts
everything.

`aggregate_results.py` checks the correctness contract from `CLAUDE.md`: every optimal
algorithm must return the same length on every instance, weighted A\* must stay inside
`w × optimal`, and greedy must be `≥ optimal`. It exits non-zero and says so if any of that
fails, because those numbers should not be reported.

Everything in this pipeline lives on shared home. `/tmp` is **node-local** on this cluster, so
a task list or instance tree under `/tmp` is invisible to the compute nodes; the array script
fails loudly rather than letting every task find no work and exit 0, which is indistinguishable
from a sweep that ran and produced nothing.

## Running experiments

This machine is the **SLURM login node**. Only builds and trivial single-instance tests run here;
all timing runs and the parameter sweep go through `sbatch`/`srun` onto compute nodes,
single-threaded per task, parallelized with job arrays.

`slurm/build_and_generate.sbatch` is the first of those jobs: it builds Release, generates the
instance set, re-reads a sample independently of the writer, and runs the verification smoke test.

```sh
mkdir -p logs && sbatch slurm/build_and_generate.sbatch
```

`GRID`, `COUNT`, `SEED`, `OUTDIR` and `SKIP_VERIFY` override the defaults via
`sbatch --export=ALL,GRID=core,COUNT=30 …`; logs land in `logs/bs-gen-<jobid>.out`.

Generating the instances is cheap — the whole job, build included, is about 9 seconds and 13 MB peak
RSS. What belongs on a compute node is the verification around it: `./build/ballsort` exhaustively
enumerates two cells' reachable state spaces and then runs BFS, bidirectional BFS, frontier BFS,
Dijkstra, A\*, IDA\* and IDDFS over a dozen instances, and that is the part that does not scale as the
grid grows.

Two cluster notes that have each cost a job:

- The `ballsort` conda env has **no plain `g++`** — only `x86_64-conda-linux-gnu-g++`, with `$CXX`
  pointing at `x86_64-conda-linux-gnu-c++`. CMake reads `$CXX` and is fine; a job script line that
  names `g++` directly dies under `set -e` before it compiles anything. Use `"$CXX"`.
- A batch shell is not a login shell, so source conda by absolute path before activating:
  `source /storage/modules/packages/anaconda/etc/profile.d/conda.sh`.
