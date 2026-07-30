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
		--array="0-${LAST}%${CONCURRENCY}" \
		--export=ALL,TASKLIST="$TASKLIST",OFFSET="$OFFSET" \
		slurm/experiment_sweep.sbatch)

	echo "chunk $c: offset $OFFSET, indices 0-$LAST -> job $JOBID"
	DEPENDENCY="$JOBID"
done

echo
echo "rows land in results/rows/, one file per (algorithm, instance, seed)."
echo "when the last chunk finishes: slurm/aggregate_results.py"
