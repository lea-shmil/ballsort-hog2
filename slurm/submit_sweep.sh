#!/bin/bash
# Submit the whole sweep as chunked job arrays.
#
# MaxArraySize on this cluster is 1001, so a task list longer than that cannot be one array.
# This splits it into consecutive arrays of at most 1000 tasks, each with an OFFSET into the
# task list, and chains them so the chunks run one after another rather than all at once.
#
# Usage:
#   bash slurm/submit_sweep.sh [TASKLIST] [CONCURRENCY]
#
#   TASKLIST     default results/tasks.tsv (from slurm/build_tasklist.py)
#   CONCURRENCY  max *cores* in flight, default 100. Per-array concurrency is derived from
#                it as CONCURRENCY/threads, so a 16-thread partition runs fewer tasks at
#                once rather than 16x the cores. The cpu128 group has 12 x 128 = 1536 cores
#                in total, so 100 is a polite neighbour; raise it if the partition is quiet.

set -euo pipefail

TASKLIST="${1:-results/tasks.tsv}"
CONCURRENCY="${2:-100}"
BATCH="${BATCH:-12}"
CHUNK=1000

# Optional per-account settings, chiefly an email address for job notifications. Gitignored,
# so it exists only in the checkout of whoever created it -- nobody else launching this sweep
# gets someone else's mail. See slurm/local.conf.example.
if [ -f slurm/local.conf ]; then
	# shellcheck disable=SC1091
	source slurm/local.conf
fi

# Environment wins over the file, so a one-off run can override without editing anything.
MAIL_USER="${MAIL_USER:-}"
MAIL_TYPE="${MAIL_TYPE:-END,FAIL}"

MAIL_ARGS=()
if [ -n "$MAIL_USER" ]; then
	# Deliberately no ARRAY_TASKS: without it, END and FAIL apply to the array as a whole,
	# so this is one message per chunk. With it, it would be one per task -- thousands.
	MAIL_ARGS=(--mail-user="$MAIL_USER" --mail-type="$MAIL_TYPE")
fi

if [ ! -f "$TASKLIST" ]; then
	echo "error: $TASKLIST not found -- run slurm/build_tasklist.py first" >&2
	exit 1
fi

TOTAL=$(grep -vc '^#' "$TASKLIST")
if [ "$TOTAL" -eq 0 ]; then
	echo "error: $TASKLIST has no tasks" >&2
	exit 1
fi

mkdir -p logs/sweep results/rows

echo "$TOTAL tasks total"
if [ -n "$MAIL_USER" ]; then
	echo "email on $MAIL_TYPE to $MAIL_USER -- one message per chunk"
else
	echo "no email (create slurm/local.conf from slurm/local.conf.example to enable)"
fi

# Split by worker count. Every algorithm but rscbt-par is serial, so reserving 16 cores for
# the whole sweep would idle fifteen of them on almost every task. One array per distinct
# thread count, each with a matching --cpus-per-task, keeps the allocation honest.
THREAD_COUNTS=$(awk -F'\t' '!/^#/ && NF>=4 {print $4}' "$TASKLIST" | sort -un)
if [ -z "$THREAD_COUNTS" ]; then
	THREAD_COUNTS=1   # task list predates the threads column
fi

DEPENDENCY=""
for T in $THREAD_COUNTS; do
	PART="${TASKLIST%.tsv}-t${T}.tsv"
	awk -F'\t' -v t="$T" '!/^#/ && NF>=4 && $4==t' "$TASKLIST" > "$PART"
	PART_TOTAL=$(grep -vc '^#' "$PART" || true)
	[ "$PART_TOTAL" -eq 0 ] && continue

	# Array tasks needed, given each one runs BATCH lines. Batching is what keeps the sweep
	# inside the cpu-part QOS ceiling of 2000 submitted jobs per user -- one job per run would
	# be 11,220 of them. See the header of experiment_sweep.sbatch.
	PART_ARRAY_TASKS=$(( (PART_TOTAL + BATCH - 1) / BATCH ))
	PART_CHUNKS=$(( (PART_ARRAY_TASKS + CHUNK - 1) / CHUNK ))

	# CONCURRENCY counts cores, not tasks: 100 single-threaded tasks and 6 sixteen-threaded
	# ones both put about 100 cores in flight. Without this, a 16-thread array at concurrency
	# 100 would ask for 1600 cores and monopolize a group that only has 1536.
	PART_CONCURRENCY=$(( CONCURRENCY / T ))
	if [ "$PART_CONCURRENCY" -lt 1 ]; then
		PART_CONCURRENCY=1
	fi
	echo "  ${T} thread(s): $PART_TOTAL runs -> $PART_ARRAY_TASKS array task(s) of $BATCH, $PART_CHUNKS chunk(s), $PART_CONCURRENCY at a time (~$(( PART_CONCURRENCY * T )) cores) -> $PART"

	for (( c=0; c<PART_CHUNKS; c++ )); do
		# OFFSET is in task-list lines; the array index then advances in units of BATCH.
		OFFSET=$(( c * CHUNK * BATCH ))
		REMAINING=$(( PART_ARRAY_TASKS - c * CHUNK ))
		LAST=$(( REMAINING < CHUNK ? REMAINING - 1 : CHUNK - 1 ))

		# Chain each chunk behind the previous ("afterany", so one failed task does not stall
		# the rest). Chaining across partitions too, so the cluster sees one sweep at a time.
		DEP_ARG=()
		if [ -n "$DEPENDENCY" ]; then
			DEP_ARG=(--dependency="afterany:$DEPENDENCY")
		fi

		JOBID=$(sbatch --parsable \
			"${DEP_ARG[@]}" \
			"${MAIL_ARGS[@]}" \
			--cpus-per-task="$T" \
			--array="0-${LAST}%${PART_CONCURRENCY}" \
			--export=ALL,TASKLIST="$PART",OFFSET="$OFFSET",THREADS="$T",BATCH="$BATCH" \
			slurm/experiment_sweep.sbatch)

		echo "    chunk $c: line offset $OFFSET, array 0-$LAST, ${T} cpu(s) -> job $JOBID"
		DEPENDENCY="$JOBID"
	done
done

echo
echo "rows land in results/rows/, one file per (algorithm, instance, seed)."
echo "when the last chunk finishes: slurm/aggregate_results.py"
