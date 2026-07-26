# ballsort-hog2

Ball-sort ("colored balls in colored tubes") domain implemented in
[HOG2](https://github.com/MovingAILab/hog2), with the best-first search family run over it for
Althaus et al., SoCS 2025. See `CLAUDE.md` for the full project brief.

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
| `include/instance_gen.h` | Shared random-instance generation, used by both binaries below |
| `environments/BallSort.h` | The domain: state, moves, goal test, hash, heuristic |
| `src/main.cpp` | `ballsort` — the domain + algorithm-family smoke test |
| `src/generate_instances.cpp` | `generate_instances` — writes reproducible instance files |

The domain and the full best-first search family from `CLAUDE.md` are wired up, and there's a
reproducible instance generator. There is not yet an experiment driver or any SLURM job script —
those are next. `./build/ballsort` runs a self-check and exits: it verifies the domain invariants
(action legality, `ApplyAction`/`UndoAction` symmetry, ball conservation, and that the state hash is
injective and invertible over the whole reachable set), then confirms that every optimal algorithm —
BFS, bidirectional BFS, frontier BFS, IDDFS/DFID, Dijkstra, A\*, IDA\* — returns the same optimal
length on several random instances, and that weighted A\* and greedy best-first respect their own
(weaker) bounds.

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

## Generating instances

`generate_instances` writes a reproducible set of random start states — every colored tube full, a
uniformly random arrangement of colors, reserve empty. Per `CLAUDE.md` every such arrangement is
solvable, so it does no solvability check; it does verify (via `instance_gen.h`) that each generated
instance has exactly the right count of each color before writing it out, since a silently corrupt
instance file would be expensive to discover after a cluster run.

```sh
./build/generate_instances --colors 4 --height 3 --count 20 --seed 7 --out instances/c4h3.txt
```

All four of `--colors`, `--height`, `--count`, `--seed` are required; `--out` defaults to stdout.
Same colors/height/count/seed always produces the same file — that's the point, since comparing
algorithms means running every one of them against the identical instance set. Generated files are
gitignored (`instances/`), since they're fully reproducible from the seed and don't need to be
tracked.

File format: `#`-prefixed header lines record colors/height/count/seed, then one instance per line —
`colors*height` space-separated integers, read as tube 1 bottom-to-top, then tube 2, and so on; the
reserve tube is implicitly empty. `generate_instances --help` shows the same summary.

This tool is deliberately independent of HOG2 and of `BallSort.h` — see the comment at the top of
`generate_instances.cpp`. Generating an instance is pure combinatorics; it doesn't need the search
machinery, so it isn't linked against it.

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

## Running experiments

This machine is the **SLURM login node**. Only builds and trivial single-instance tests run here;
all timing runs and the parameter sweep go through `sbatch`/`srun` onto compute nodes,
single-threaded per task, parallelized with job arrays.
