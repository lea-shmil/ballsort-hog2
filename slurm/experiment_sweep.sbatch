#!/bin/bash
#SBATCH --job-name=bs-sweep
#SBATCH --partition=cpu
#SBATCH --constraint=cpu128
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=60G
#SBATCH --time=12:00:00
#SBATCH --output=logs/sweep/%A_%a.out
#SBATCH --error=logs/sweep/%A_%a.out
#
# One array task = one (algorithm, instance) pair = one CSV row.
#
# **Pinned to one machine type.** `--constraint=cpu128` restricts the sweep to the twelve
# ise-cpu128-* nodes, which are all AMD EPYC 7702P at the same max clock (verified, not
# assumed). This matters because runtime is one of the four metrics the paper reports: the
# `cpu` partition also holds cs-cpu-* boxes with different core counts and the ise-cpu256-*
# machines, so an unpinned array would spread tasks across CPU generations and the
# wall-clock column would be measuring hardware rather than algorithms. Node counts and
# solution lengths are hardware-independent and would survive; the timings would not.
#
# Every row also records the node it ran on and that node's CPU model, so any residual
# heterogeneity is visible in the data instead of being assumed away. Override the pinning
# with CONSTRAINT= if a different homogeneous group is wanted:
#   sbatch --constraint=cpu256 ...
#
# One pair per task is the whole point: tasks are independent, a task that exceeds a limit is
# killed by SLURM without touching anything else, and a killed run is a *result* -- a timeout
# or an exhausted memory budget, which is what the paper's empty Figure 6 boxes are -- rather
# than a lost run. CLAUDE.md's "single-threaded per task, parallelize with job arrays" is
# exactly this shape.
#
# **Memory is set per algorithm by submit_sweep.sh, which overrides the directive above.**
# Measured over 9,817 runs: the closed-list searches (bfs, bibfs, dijkstra, frontier-bfs,
# astar-*) peak at 15-32 GB and keep climbing with the cell, while the other twelve
# algorithms never exceed 0.5 GB. An earlier sweep gave everything 32 GB and lost 258 array
# tasks to the OOM killer on cells that would have fitted in a larger allocation -- 13x3
# needs about 164 GB and 6x4 about 216 GB at the ~150 bytes per state these searches use.
# build_tasklist.py now tags each task with its memory, and submit_sweep.sh submits one array
# per (threads, memory) profile.
#
# The directive is the fallback for a direct submission. 60 GB is the practical ceiling here:
# the cpu128 nodes hold 514 GB but run mostly allocated to other users, so larger requests are
# accepted and then pend on Resources rather than running. It also leaves the search room
# beyond its closed list rather than sizing the request to the whole node.
#
# More memory would not make every cell fit and is not meant to: exhaustive search needs about
# 164 GB on 13x3, 216 GB on 6x4, 660 GB on 4x5, 1.4 TB on 3x6, 3.7 TB on 2x8 and 7.2 TB on
# 7x4. On those, running out of memory is the finding.
#
# **Each array task runs a batch of consecutive task-list lines**, not just one. That is
# forced by the cluster's QOS: `cpu-part` allows a user at most 2000 submitted jobs, and every
# array task counts as one, so an 11,220-run sweep cannot be 11,220 jobs. With BATCH lines per
# task the whole sweep is a few hundred jobs, comfortably inside both that limit and the
# MaxArraySize of 1001.
#
# Batching does not weaken the per-run limits, which is the point: each run inside the batch
# gets its own RUN_TIMEOUT via timeout(1), so one pathological instance is killed on its own
# rather than consuming the batch's budget. A killed run leaves no row, which is exactly how a
# timeout was already recorded. The task's own --time has to cover BATCH runs, hence 12h.
#
# Do not sbatch this directly. Use:
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

BATCH="${BATCH:-12}"
RUN_TIMEOUT="${RUN_TIMEOUT:-3600}"

# This task owns lines [FIRST, FIRST+BATCH) of the task list, 1-based.
FIRST=$((OFFSET + SLURM_ARRAY_TASK_ID*BATCH + 1))

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

echo "array task $SLURM_ARRAY_TASK_ID: lines $FIRST..$((FIRST+BATCH-1)) of $TOTAL_TASKS in $TASKLIST"
echo "node: $(hostname)  ${SLURM_CPUS_PER_TASK:-1} cpu(s), ${SLURM_MEM_PER_NODE:-?} MB, ${RUN_TIMEOUT}s per run"

source /storage/modules/packages/anaconda/etc/profile.d/conda.sh
conda activate ballsort

mkdir -p "$ROWDIR"

DONE=0
TIMED_OUT=0
OOM=0
FAILED=0

for (( n=0; n<BATCH; n++ )); do
	LINE_NUMBER=$((FIRST + n))
	# Skip comment lines when numbering, so line N is task N regardless of the header.
	TASK=$(grep -v '^#' "$TASKLIST" | sed -n "${LINE_NUMBER}p")
	if [ -z "$TASK" ]; then
		break          # past the end: the last array task is a short batch
	fi

	ALGORITHM=$(printf '%s' "$TASK" | cut -f1)
	INSTANCE=$(printf '%s' "$TASK" | cut -f2)
	SEED_LABEL=$(printf '%s' "$TASK" | cut -f3)
	# Workers for rscbt-par; every other algorithm is serial and ignores this. submit_sweep.sh
	# gives each thread-count partition its own array with a matching --cpus-per-task.
	TASK_THREADS=$(printf '%s' "$TASK" | cut -f4)
	TASK_THREADS="${TASK_THREADS:-${THREADS:-1}}"

	# One file per row rather than appending to a shared CSV: many concurrent array tasks
	# appending to one file would interleave and corrupt lines. aggregate_results.py concatenates.
	SAFE_INSTANCE=$(printf '%s' "$INSTANCE" | tr '/' '_')
	ROW="$ROWDIR/${ALGORITHM}__${SAFE_INSTANCE}__seed${SEED_LABEL}.csv"

	# Each run is capped on its own, so one pathological instance cannot eat the batch's
	# budget and starve the runs queued behind it in this task.
	#
	# Plain SIGTERM, deliberately: `timeout --signal=KILL` makes a timeout exit 137, which is
	# exactly what the kernel OOM killer produces, and the two outcomes then cannot be told
	# apart. With the default signal a timeout exits 124 and only an OOM kill exits 137, which
	# is the distinction the paper draws when it marks runs that "did not finish (due to
	# running out of memory)". --kill-after is the backstop for a run that ignores SIGTERM.
	RUN_START=$(date +%s)
	timeout --kill-after=60 "$RUN_TIMEOUT" ./build/run_experiment \
		--algorithm "$ALGORITHM" \
		--instance "$INSTANCE" \
		--seed-label "$SEED_LABEL" \
		--timing-repeats "$TIMING_REPEATS" \
		--threads "$TASK_THREADS" \
		> "$ROW"
	STATUS=$?

	RUN_ELAPSED=$(( $(date +%s) - RUN_START ))

	if [ $STATUS -eq 0 ]; then
		DONE=$((DONE+1))
		cat "$ROW"
	else
		# No row survives a kill, and a missing row is how aggregate_results.py records a run
		# that did not finish. The *reason* is logged here so timeouts and memory exhaustion
		# stay distinguishable in the record:
		#   124 -- timeout(1) fired, so the run exceeded RUN_TIMEOUT
		#   137 -- SIGKILL, which with plain SIGTERM timeouts means the OOM killer
		#   other -- a real failure (bad arguments, unparsable instance)
		rm -f "$ROW"
		case $STATUS in
			124)
				TIMED_OUT=$((TIMED_OUT+1))
				echo "KILLED reason=timeout elapsed=${RUN_ELAPSED}s limit=${RUN_TIMEOUT}s algorithm=$ALGORITHM instance=$INSTANCE seed=$SEED_LABEL"
				;;
			137)
				OOM=$((OOM+1))
				echo "KILLED reason=out-of-memory elapsed=${RUN_ELAPSED}s limit=${SLURM_MEM_PER_NODE:-?}MB algorithm=$ALGORITHM instance=$INSTANCE seed=$SEED_LABEL"
				;;
			*)
				FAILED=$((FAILED+1))
				echo "KILLED reason=failed status=$STATUS elapsed=${RUN_ELAPSED}s algorithm=$ALGORITHM instance=$INSTANCE seed=$SEED_LABEL"
				;;
		esac
	fi
done

echo "batch finished: $DONE completed, $TIMED_OUT timed out, $OOM out of memory, $FAILED failed"
