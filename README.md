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
| `src/main.cpp` | Entry point; currently a build smoke test |

`environments/` is on the include path in `CMakeLists.txt` but does not exist yet — the domain
header `BallSort.h` has not been written. Nothing is implemented so far: there is no domain, no
search algorithms, no instance generator. The binary prints one line and exits.

Only five HOG2 directories are on the include path — `search`, `generic`, `algorithms`, `utils`,
`simulation`. The build is headless: no `gui/`, no SFML, no OpenGL.

Note that the algorithm headers do not live where the directory names suggest: `BFS.h`,
`FrontierBFS.h`, `UnitCostBidirectionalBFS.h`, `DFID.h`, `IDAStar.h` and `TemplateAStar.h` are all in
`hog2/generic/`, while `hog2/search/` holds `SearchEnvironment.h` and the PDB machinery.

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
