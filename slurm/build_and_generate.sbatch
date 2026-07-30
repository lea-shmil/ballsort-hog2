#!/bin/bash
#SBATCH --job-name=bs-gen
#SBATCH --partition=main
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --time=00:30:00
#SBATCH --output=logs/%x-%j.out
#SBATCH --error=logs/%x-%j.out
#
# Build the project, generate the instance set, and run the verification smoke test --
# all on a compute node.
#
# Everything here used to be run inline on the login node, and the verification step is
# what makes that a bad idea: ./build/ballsort exhaustively enumerates the reachable state
# space of two cells and then runs BFS, bidirectional BFS, frontier BFS, Dijkstra, A*,
# IDA* and IDDFS over a dozen instances. Generating the 530 instance files is trivially
# cheap by comparison; the searching around it is not, and it took the login node down.
# Per CLAUDE.md the login node gets builds and trivial single-instance tests only, so this
# is the right home for the whole sequence.
#
# The four CPUs are for the parallel compile only. Every search here is single-threaded,
# and the real timing sweep will parallelize with a job array rather than with threads.
#
# Usage (from the repo root):
#   mkdir -p logs && sbatch slurm/build_and_generate.sbatch
#
# Optional overrides, as environment variables:
#   GRID=paper|runnable|core   which cell set to generate (default paper -- the full
#                              published grid; core is the subset intended for the
#                              algorithm comparison, and is byte-identical to the
#                              corresponding paper-grid files at the same seed)
#   COUNT=N                    instances per cell (default 10, the paper's protocol)
#   SEED=S                     master seed (default is the tool's own default)
#   OUTDIR=DIR                 output directory (default instances)
#   SKIP_VERIFY=1              build and generate, but skip the search-heavy smoke test
#
#   sbatch --export=ALL,GRID=core,COUNT=30 slurm/build_and_generate.sbatch

set -euo pipefail

GRID="${GRID:-paper}"
COUNT="${COUNT:-10}"
OUTDIR="${OUTDIR:-instances}"

# The repo root is wherever sbatch was invoked from; be explicit rather than relying on it.
REPO_ROOT="${SLURM_SUBMIT_DIR:-$PWD}"
cd "$REPO_ROOT"

echo "=== job $SLURM_JOB_ID on $(hostname) ==="
echo "repo:    $REPO_ROOT"
echo "grid:    $GRID"
echo "count:   $COUNT per cell"
echo "outdir:  $OUTDIR"
echo "started: $(date -Is)"
echo

# This cluster has no system g++ or cmake and no gcc/cmake modules -- the toolchain comes
# from the ballsort conda env (see README). Source conda by absolute path: a batch shell
# is not a login shell, so nothing has set conda up for us.
#
# There is no plain `g++` in that env -- conda's compiler package installs it as
# x86_64-conda-linux-gnu-g++ and points $CXX at x86_64-conda-linux-gnu-c++, which is how
# cmake finds it. Report $CXX, not g++, or this script dies before it ever compiles.
source /storage/modules/packages/anaconda/etc/profile.d/conda.sh
conda activate ballsort
echo "cmake: $(command -v cmake)"
echo "CXX:   ${CXX:-<unset>}"
"${CXX:?the ballsort env should export CXX}" --version | head -1
echo

echo "=== build (Release) ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "${SLURM_CPUS_PER_TASK:-4}"
echo

echo "=== generate instances ==="
GEN_ARGS=(--grid "$GRID" --outdir "$OUTDIR" --count "$COUNT")
if [ -n "${SEED:-}" ]; then
	GEN_ARGS+=(--seed "$SEED")
fi
./build/generate_instances "${GEN_ARGS[@]}"
echo
echo "instance files written: $(find "$OUTDIR" -name '*.in' | wc -l)"
echo "cells:                 $(find "$OUTDIR" -mindepth 1 -maxdepth 1 -type d | wc -l)"
echo

# generate_instances already reads back every file it writes, so this is a second,
# independent pass: parse the files with no knowledge of what was supposed to be in them
# and report the per-instance difficulty tags. Also the code path that ingests the paper's
# own published resources/paper_inputs/*.in, which use this same format.
echo "=== re-read a sample, independent of the writer ==="
find "$OUTDIR" -name '*_0.in' | sort | head -8 | xargs ./build/generate_instances --read
echo

if [ "${SKIP_VERIFY:-0}" = "1" ]; then
	echo "=== verification smoke test SKIPPED (SKIP_VERIFY=1) ==="
else
	echo "=== verification smoke test (search-heavy; this is why the job exists) ==="
	/usr/bin/time -v ./build/ballsort 2>&1 || {
		echo "SMOKE TEST FAILED"
		exit 1
	}
fi

echo
echo "finished: $(date -Is)"
