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
#
#   NOMAIL=1     suppress job notifications entirely. Always set this for trial or debug
#                submissions -- cancelling one otherwise emails a FAIL that is
#                indistinguishable from a real sweep failure.
#   MEM_BUDGET_GB  max memory in flight across a partition, default 720 GB -- about one
#                memory-heavy task per cpu128 node. Concurrency is the smaller of the core
#                and memory budgets, so the 60 GB closed-list arrays self-limit to a dozen.
#                The QOS would allow 4 TB, but the nodes are mostly allocated to other users,
#                so a budget sized to the QOS pends instead of running.
#   CONCURRENCY  max *cores* in flight, default 100. Per-array concurrency is derived from
#                it as CONCURRENCY/threads, so a 16-thread partition runs fewer tasks at
#                once rather than 16x the cores. The cpu128 group has 12 x 128 = 1536 cores
#                in total, so 100 is a polite neighbour; raise it if the partition is quiet.

set -euo pipefail

TASKLIST="${1:-results/tasks.tsv}"
CONCURRENCY="${2:-100}"
BATCH="${BATCH:-12}"
# Memory budget in GB for the whole sweep, capping how many memory-hungry tasks run at once.
# The cpu-part QOS allows a user mem=4T, so 3000 GB leaves headroom; without this cap a
# 240 GB partition at concurrency 100 would ask for 24 TB and simply never schedule.
# Roughly one memory-heavy task per cpu128 node. The QOS would allow 4 TB, but the nodes run
# with most of their memory already allocated to other users, so a budget sized to the QOS
# would simply pend. Raise it when the partition is quiet.
MEM_BUDGET_GB="${MEM_BUDGET_GB:-720}"
CHUNK=1000

# Optional per-account settings, chiefly an email address for job notifications. Gitignored,
# so it exists only in the checkout of whoever created it -- nobody else launching this sweep
# gets someone else's mail. See slurm/local.conf.example.
#
# The environment has to be captured *before* sourcing, because local.conf assigns
# unconditionally and would otherwise clobber it. Without this, `MAIL_USER= bash
# submit_sweep.sh` silently still sends mail -- which is exactly how four cancelled trial
# submissions ended up in someone's inbox.
MAIL_USER_ENV="${MAIL_USER-__unset__}"
MAIL_TYPE_ENV="${MAIL_TYPE-__unset__}"

if [ -f slurm/local.conf ]; then
	# shellcheck disable=SC1091
	source slurm/local.conf
fi

if [ "$MAIL_USER_ENV" != "__unset__" ]; then
	MAIL_USER="$MAIL_USER_ENV"
fi
if [ "$MAIL_TYPE_ENV" != "__unset__" ]; then
	MAIL_TYPE="$MAIL_TYPE_ENV"
fi
MAIL_USER="${MAIL_USER:-}"
MAIL_TYPE="${MAIL_TYPE:-BEGIN,END,FAIL}"

# NOMAIL=1 forces silence regardless of the config. Use it for every trial or debug
# submission: a cancelled trial otherwise sends a FAIL notice that looks exactly like a real
# sweep failing.
if [ -n "${NOMAIL:-}" ]; then
	MAIL_USER=""
fi

MAIL_ARGS=()
if [ -n "$MAIL_USER" ]; then
	# Deliberately no ARRAY_TASKS: without it, BEGIN, END and FAIL apply to the array as a
	# whole, so this is a handful of messages. With it, it would be one per array task --
	# thousands.
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

# Split by (worker count, memory). Only rscbt-par wants more than one core, and only the
# closed-list searches want more than a few GB, so a single allocation for the whole sweep
# would either starve those or waste an order of magnitude of concurrency on the twelve
# algorithms that peak under 0.5 GB. One array per distinct (threads, memory) pair, each with
# matching --cpus-per-task and --mem, keeps the allocation honest in both directions.
PROFILES=$(awk -F'\t' '!/^#/ && NF>=5 {print $4"_"$5}' "$TASKLIST" | sort -u)
if [ -z "$PROFILES" ]; then
	# Task list predates the memory column: fall back to thread counts at the sbatch default.
	PROFILES=$(awk -F'\t' '!/^#/ && NF>=4 {print $4"_default"}' "$TASKLIST" | sort -u)
fi
if [ -z "$PROFILES" ]; then
	PROFILES="1_default"
fi

DEPENDENCY=""
for PROFILE in $PROFILES; do
	T="${PROFILE%%_*}"
	MEM_GB="${PROFILE##*_}"

	MEM_ARG=()
	if [ "$MEM_GB" != "default" ]; then
		MEM_ARG=(--mem="${MEM_GB}G")
		PART="${TASKLIST%.tsv}-t${T}m${MEM_GB}.tsv"
		awk -F'\t' -v t="$T" -v m="$MEM_GB" '!/^#/ && NF>=5 && $4==t && $5==m' "$TASKLIST" > "$PART"
	else
		PART="${TASKLIST%.tsv}-t${T}.tsv"
		awk -F'\t' -v t="$T" '!/^#/ && NF>=4 && $4==t' "$TASKLIST" > "$PART"
	fi
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

	# ...and by memory, for the same reason in the other dimension. The nodes are largely
	# allocated to other users, so an unbounded number of 60 GB tasks would pend, not run.
	if [ "$MEM_GB" != "default" ]; then
		MEM_CONCURRENCY=$(( MEM_BUDGET_GB / MEM_GB ))
		if [ "$MEM_CONCURRENCY" -lt 1 ]; then
			MEM_CONCURRENCY=1
		fi
		if [ "$MEM_CONCURRENCY" -lt "$PART_CONCURRENCY" ]; then
			PART_CONCURRENCY=$MEM_CONCURRENCY
		fi
	fi
	echo "  ${T} thread(s), ${MEM_GB} GB: $PART_TOTAL runs -> $PART_ARRAY_TASKS array task(s) of $BATCH, $PART_CHUNKS chunk(s), $PART_CONCURRENCY at a time (~$(( PART_CONCURRENCY * T )) cores) -> $PART"

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
			"${MEM_ARG[@]}" \
			--array="0-${LAST}%${PART_CONCURRENCY}" \
			--export=ALL,TASKLIST="$PART",OFFSET="$OFFSET",THREADS="$T",BATCH="$BATCH" \
			slurm/experiment_sweep.sbatch)

		echo "    chunk $c: line offset $OFFSET, array 0-$LAST, ${T} cpu(s), ${MEM_GB} GB -> job $JOBID"
		DEPENDENCY="$JOBID"
	done
done

echo
echo "rows land in results/rows/, one file per (algorithm, instance, seed)."
echo "when the last chunk finishes: slurm/aggregate_results.py"
