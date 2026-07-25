# Project Brief: Ball-Sort Domain in HOG2

Implement the "colored balls in colored tubes" domain in HOG2 and run the **best-first search family**
over it — from `h = 0` (BFS/Dijkstra) through A\*, IDA\*, and weighted A\* with a real heuristic.
The output is the search-algorithm comparison for **Althaus et al., SoCS 2025**.

## Domain model (faithful to Althaus)

- Colors `1..c`.
- Tubes: `T0` = uncolored reserve, plus `T1..Tc` = one goal tube per color. All tubes have the same height `h`.
- Start state: reserve `T0` empty; every colored tube full.
- Move `(i, j)`: move the top ball of tube `i` onto tube `j`. Valid **iff** source is non-empty **and** destination is not full.
  - **No color matching** — any top ball may go onto any non-full tube.
  - **No per-tube color restriction.**
- Goal: reserve empty, and each `Ti` full of color `i`.
- Solvability: any full-tube start with exactly `h` balls per color is solvable, so generated instances need **no solvability check**.

## HOG2 integration (headless)

- No GUI: do **not** build `gui/` or the SFML tree; no OpenGL.
- Vendor `MovingAILab/hog2`, branch **`PDB-refactor`**, as a git submodule at `./hog2`.
- CMake include dirs — **exactly these five**:
  - `hog2/search`
  - `hog2/generic`
  - `hog2/algorithms`
  - `hog2/utils`
  - `hog2/simulation`
- New domain header `environments/BallSort.h`, modeled on `hog2/environments/TOH.h` but **without** including `PDBHeuristic.h`. The A* heuristic is hand-written (a `Heuristic<BallSortState>` subclass or `HCost` on the environment), not a PDB — so this exclusion still holds.
- `GetStateHash` must be a **perfect, injective bit-packing** in the style of TOH's. Also provide `GetStateFromHash` and `GetMaxHash`, so A* runs correctly unmodified.

## Algorithms to compare

Zero-heuristic end of the family (`h = 0`) — the reference point, not the end goal:

- BFS
- Bidirectional BFS
- Frontier BFS
- IDDFS / DFID
- Dijkstra (i.e. A* with `h = 0`)

Heuristic-guided:

- **A\* with a real heuristic** — either taken from the Althaus et al. paper the problem is based on, or written by us if the paper has none usable. Must be **admissible and consistent** so A* stays optimal.
  - Open item: read the paper first and lift its bound if there is one.
  - Fallback candidate (ours, needs proof of admissibility before use): count of balls not in their goal tube, since each such ball needs ≥1 move. Refine from there.
- **IDA\*** with the same heuristic — the linear-space counterpart to A\*, and the natural informed pair to IDDFS/DFID.
- **Weighted A\*** with the same heuristic, swept over weights `w > 1`.
- **Greedy best-first search** with the same heuristic — orders on `h` alone, ignoring `g`. The far end of the weight sweep (`w = ∞`).

Correctness contract:

- Every **optimal** algorithm above (BFS, bidirectional BFS, frontier BFS, IDDFS/DFID, Dijkstra, A\*, IDA\*) must return the **same optimal solution length** on every instance. This is the cross-check that the domain and the heuristic are correct.
- **Weighted A\* and greedy best-first are exempt** — both are expected to return solutions of length ≥ optimal. Record their solution lengths alongside the optimal one and report the suboptimality ratio (for weighted A\*, check it against the `w` bound; greedy best-first has no bound).

## Cluster rules (SLURM)

- This machine is the **SLURM login node**.
- Inline here: builds and trivial single-instance tests only.
- Everything else — all real timing runs and the parameter sweep — goes through `sbatch`/`srun` onto compute nodes.
- Compile **Release** (`-O2`/`-O3`).
- **Single-threaded per task**; parallelize with **job arrays**.
