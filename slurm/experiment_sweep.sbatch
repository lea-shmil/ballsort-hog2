#!/bin/bash
#SBATCH --job-name=bs-sweep
#SBATCH --partition=main
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=32G
#SBATCH --time=01:00:00
#SBATCH --output=logs/sweep/%A_%a.out
#SBATCH --error=logs/sweep/%A_%a.out
#
# One array task = one (algorithm, instance) pair = one CSV row.
#
# 60 minutes and 32 GB per task, single-threaded, as agreed. One pair per task is the whole
# point: tasks are independent, a task that exceeds either limit is killed by SLURM without
# touching anything else, and a killed task is a *result* -- a timeout, which is what the
# paper's empty Figure 6 boxes are -- rather than a lost run. CLAUDE.md's "single-threaded
# per task, parallelize with job arrays" is exactly this shape.
#
# Do not sbatch this directly: MaxArraySize on this cluster is 1001 and the sweep is larger,
# so submission is chunked. Use:
#
#   ./build/generate_instances --grid core --outdir instances   # or let the next line do it
#   slurm/build_tasklist.py --grid core --seeds 20250726,20250727,20250728
#   bash slurm/submit_sweep.sh
#
# TASKLIST and OFFSET come from submit_sweep.sh; SLURM_ARRAY_TASK_ID selects the line.

set -uo pipefail

REPO_ROOT="${SLURM_SUBMIT_DIR:-$PWD}"
cd "$REPO_ROOT"

TASKLIST="${TASKLIST:-results/tasks.tsv}"
OFFSET="${OFFSET:-0}"
TIMING_REPEATS="${TIMING_REPEATS:-3}"
ROWDIR="${ROWDIR:-results/rows}"

LINE_NUMBER=$((OFFSET + SLURM_ARRAY_TASK_ID + 1))

# Fail loudly if the task list is not visible from this node. An unreadable task list makes
# every array task find no work and exit 0, which looks exactly like a sweep that ran and
# produced nothing -- the worst possible failure mode here. The usual cause is a path on
# node-local storage: /tmp is per-node on this cluster, so the task list, the instances and
# the output directory all have to live on shared home.
if [ ! -r "$TASKLIST" ]; then
	echo "FATAL: task list '$TASKLIST' is not readable from $(hostname)." >&2
	echo "       If it is under /tmp, that is node-local -- put it on shared storage." >&2
	exit 2
fi

TOTAL_TASKS=$(grep -vc '^#' "$TASKLIST")

# Skip comment lines when numbering, so line N is task N regardless of the header.
TASK=$(grep -v '^#' "$TASKLIST" | sed -n "${LINE_NUMBER}p")
if [ -z "$TASK" ]; then
	# Genuinely past the end: the last chunk's array is padded to a round size.
	echo "no task at line $LINE_NUMBER of $TOTAL_TASKS (offset $OFFSET + index $SLURM_ARRAY_TASK_ID) -- past the end, nothing to do"
	exit 0
fi

ALGORITHM=$(printf '%s' "$TASK" | cut -f1)
INSTANCE=$(printf '%s' "$TASK" | cut -f2)
SEED_LABEL=$(printf '%s' "$TASK" | cut -f3)

echo "task $SLURM_ARRAY_TASK_ID (line $LINE_NUMBER): $ALGORITHM on $INSTANCE (seed $SEED_LABEL)"
echo "node: $(hostname)  limits: ${SLURM_MEM_PER_NODE:-?} MB / $(squeue -h -j "$SLURM_JOB_ID" -o %l 2>/dev/null)"

source /storage/modules/packages/anaconda/etc/profile.d/conda.sh
conda activate ballsort

mkdir -p "$ROWDIR"

# One file per row rather than appending to a shared CSV: hundreds of concurrent array tasks
# appending to one file will interleave and corrupt lines. aggregate_results.py concatenates.
SAFE_INSTANCE=$(printf '%s' "$INSTANCE" | tr '/' '_')
ROW="$ROWDIR/${ALGORITHM}__${SAFE_INSTANCE}__seed${SEED_LABEL}.csv"

./build/run_experiment \
	--algorithm "$ALGORITHM" \
	--instance "$INSTANCE" \
	--seed-label "$SEED_LABEL" \
	--timing-repeats "$TIMING_REPEATS" \
	> "$ROW"
STATUS=$?

if [ $STATUS -ne 0 ]; then
	# A non-zero exit here is a real failure (bad arguments, unparsable instance). A timeout
	# or an out-of-memory kill never reaches this line -- SLURM kills the task, the row file
	# is left empty or absent, and aggregate_results.py records that as a timeout.
	echo "run_experiment failed with status $STATUS"
	rm -f "$ROW"
	exit $STATUS
fi

cat "$ROW"
