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
#   CONCURRENCY  max simultaneously running tasks per chunk, default 100. Raise it if the
#                partition is quiet, lower it to be a better neighbour.

set -euo pipefail

TASKLIST="${1:-results/tasks.tsv}"
CONCURRENCY="${2:-100}"
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

CHUNKS=$(( (TOTAL + CHUNK - 1) / CHUNK ))
echo "$TOTAL tasks, $CHUNKS chunk(s) of up to $CHUNK, concurrency $CONCURRENCY per chunk"
if [ -n "$MAIL_USER" ]; then
	echo "email on $MAIL_TYPE to $MAIL_USER -- one message per chunk, so $CHUNKS in total"
else
	echo "no email (create slurm/local.conf from slurm/local.conf.example to enable)"
fi

DEPENDENCY=""
for (( c=0; c<CHUNKS; c++ )); do
	OFFSET=$(( c * CHUNK ))
	REMAINING=$(( TOTAL - OFFSET ))
	LAST=$(( REMAINING < CHUNK ? REMAINING - 1 : CHUNK - 1 ))

	# Chain chunk c+1 behind chunk c ("afterany", so one failed task does not stall the rest).
	DEP_ARG=()
	if [ -n "$DEPENDENCY" ]; then
		DEP_ARG=(--dependency="afterany:$DEPENDENCY")
	fi

	JOBID=$(sbatch --parsable \
		"${DEP_ARG[@]}" \
		"${MAIL_ARGS[@]}" \
		--array="0-${LAST}%${CONCURRENCY}" \
		--export=ALL,TASKLIST="$TASKLIST",OFFSET="$OFFSET" \
		slurm/experiment_sweep.sbatch)

	echo "chunk $c: offset $OFFSET, indices 0-$LAST -> job $JOBID"
	DEPENDENCY="$JOBID"
done

echo
echo "rows land in results/rows/, one file per (algorithm, instance, seed)."
echo "when the last chunk finishes: slurm/aggregate_results.py"
