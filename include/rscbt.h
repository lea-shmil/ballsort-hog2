// rscbt.h -- the RSCBT algorithm of Althaus et al., SoCS 2025 (their Algorithm 1).
//
// ATTRIBUTION. The algorithm is theirs. This is our reimplementation against HOG2's
// SearchEnvironment interface, following the description in their §6 and the structure of
// their `apps/serial_bfs`; none of their code is vendored (see the README's provenance
// section). The lower bound it calls is dfvs_bound.h, which is their bound.
//
// What it is, in one line: a *breadth-first* search over configurations, layer by layer,
// where every generated configuration is filtered against an upper bound mu using an
// admissible lower bound. Their §6 is explicit that the order is breadth-first and not
// best-first -- "although this could lead to enumerating more vertices [...] it allows for
// easier parallelization" -- so it is not A*, and it is not frontier BFS either. It sits
// where a breadth-first branch-and-bound sits: linear-ish in memory like frontier search,
// f-bounded like IDA*, but expanding in g order rather than f order.
//
// Their Algorithm 1, transcribed:
//
//   given mu, an upper bound on the number of moves
//   level_0 = {start}, level_{-1} = {}
//   for i = 0, 1, 2, ...:
//     inBound = {}, outOfBound = {}
//     for each S' in level_i:
//       lb = LOWER-BOUND(S')
//       if lb == 0: return TRUE                     # S' is final, reached in i moves
//       N = NEIGHBORS(S') \ (level_i union level_{i-1})
//       if lb + i > mu: outOfBound |= N             # nothing through S' can make it
//       else:           inBound    |= N
//     level_{i+1} = inBound \ outOfBound
//     if level_{i+1} is empty: return FALSE
//     level_{i-1} = level_i; level_i = level_{i+1}
//
// Two details worth not losing:
//
//  - Only three layers are ever held: i-1, i, i+1. Excluding the previous layer is what
//    replaces a closed list, exactly as in frontier search, and is correct because the
//    graph is undirected in the sense that every move is invertible (BallSort::InvertAction),
//    so a shortest path never revisits a node two layers back.
//  - The out-of-bound set and the final set difference are not an optimization. A
//    configuration T can be generated from two parents in the same layer, one pruned and
//    one not; their §6 spells out that the pruning knowledge must win, so pruned successors
//    are collected separately and subtracted at the end of the layer rather than simply
//    skipped.
//
// Algorithm 1 is a *decision* procedure: "is there a solution within mu moves". Getting the
// optimum means iterating mu, which RSCBTSolve does from the root lower bound upward -- so
// the first mu that answers TRUE is the optimal length. That upward sweep is what makes the
// whole thing comparable to IDA*, and it means a run costs the sum over all failed bounds,
// which is the price of the linear memory.

#ifndef BALLSORT_RSCBT_H
#define BALLSORT_RSCBT_H

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_set>
#include <vector>

#include "BallSort.h"
#include "dfvs_bound.h"

/**
 * Canonical representative of a configuration's color-permutation equivalence class -- the
 * symmetry reduction of their §6.
 *
 * Their wording: "Two configurations are equivalent if they can be transformed into each
 * other by a permutation of the colors, which is a recoloring of the balls and a reordering
 * of the tubes accordingly. From all tube-rack configurations in an equivalence class we
 * select the one with the lexicographically smallest bitset representation."
 *
 * Sound only because the goal is itself invariant under the same group: tube i full of
 * color i maps to tube pi(i) full of color pi(i), which is the goal again. So every member
 * of a class is equidistant from the goal, and collapsing to representatives preserves
 * optimal lengths. The lower bound in dfvs_bound.h is likewise invariant, since relabeling
 * colors only renames the graph's vertices.
 *
 * They note the representative can be built directly from any member; this minimizes over
 * all numColors! relabelings, which is the straightforward version and costs c! per state
 * (5040 at c=7). That is why RSCBTSolve takes it as a flag: it shrinks the state space but
 * makes node counts incomparable with the other algorithms', so the comparison wants both.
 */
template <int numColors, int tubeHeight>
BallSortState<numColors, tubeHeight> RSCBTRepresentative(const BallSortState<numColors, tubeHeight> &s)
{
	typedef BallSortState<numColors, tubeHeight> State;

	std::vector<int> perm(numColors);
	std::iota(perm.begin(), perm.end(), 1);

	State best;
	bool haveBest = false;

	do {
		// perm[i-1] is the new name of color i, and tube i's contents move to tube perm[i-1].
		State candidate;
		for (int t = 0; t <= numColors; t++)
		{
			candidate.counts[t] = 0;
			for (int k = 0; k < tubeHeight; k++)
				candidate.balls[t][k] = 0;
		}

		// The reserve stays the reserve, keeping its stack order, with its balls recolored.
		candidate.counts[0] = s.counts[0];
		for (int k = 0; k < s.GetBallCountInTube(0); k++)
			candidate.balls[0][k] = static_cast<uint8_t>(perm[s.GetBallInTube(0, k)-1]);

		for (int t = 1; t <= numColors; t++)
		{
			const int target = perm[t-1];
			candidate.counts[target] = s.counts[t];
			for (int k = 0; k < s.GetBallCountInTube(t); k++)
				candidate.balls[target][k] = static_cast<uint8_t>(perm[s.GetBallInTube(t, k)-1]);
		}

		if (!haveBest)
		{
			best = candidate;
			haveBest = true;
			continue;
		}

		// Lexicographic order over the packed slots -- our stand-in for their "smallest
		// bitset representation". Any fixed total order picks a well-defined representative;
		// only consistency matters, not which member wins.
		for (int slot = 0; slot < (numColors+1)*tubeHeight; slot++)
		{
			const int t = slot/tubeHeight, k = slot%tubeHeight;
			if (candidate.balls[t][k] != best.balls[t][k])
			{
				if (candidate.balls[t][k] < best.balls[t][k])
					best = candidate;
				break;
			}
		}
	} while (std::next_permutation(perm.begin(), perm.end()));

	return best;
}

struct RSCBTResult {
	int solutionLength = -1;        // optimal number of moves, or -1 if not found
	uint64_t nodesExpanded = 0;     // configurations taken off a layer and expanded
	uint64_t nodesGenerated = 0;    // successors produced, before deduplication
	uint64_t maxElements = 0;       // largest number of configurations held at once --
									// the paper's Figure 6c metric
	int iterations = 0;             // how many values of mu were tried
	bool exhausted = false;         // a mu round proved FALSE (no solution within it)
};

/**
 * One round of Algorithm 1: is `start` solvable within `mu` moves?
 *
 * Returns the number of moves if a final configuration is reached at some layer i <= mu,
 * otherwise -1. Statistics accumulate into `result` across rounds.
 */
template <int numColors, int tubeHeight>
int RSCBTDecide(const BallSort<numColors, tubeHeight> &env,
				const BallSortDFVSHeuristic<numColors, tubeHeight> &bound,
				const BallSortState<numColors, tubeHeight> &start,
				int mu, bool symmetryReduction, RSCBTResult &result)
{
	typedef BallSortState<numColors, tubeHeight> State;
	const State goal = env.GetGoalState();

	// With symmetry reduction on, every configuration entering a layer is replaced by its
	// class representative, so the layers hold one member per class instead of all c! of them.
	auto canonical = [&](const State &s) -> State {
		return symmetryReduction ? RSCBTRepresentative<numColors, tubeHeight>(s) : s;
	};

	std::unordered_set<uint64_t> previous, current, inBound, outOfBound;
	std::vector<State> currentStates, nextStates;

	const State canonicalStart = canonical(start);
	current.insert(env.GetStateHash(canonicalStart));
	currentStates.push_back(canonicalStart);

	std::vector<BallSortMove> actions;

	for (int i = 0; i <= mu; i++)
	{
		inBound.clear();
		outOfBound.clear();
		nextStates.clear();
		// Successors are kept as states alongside their hashes: the layer needs the states
		// to expand next round, and the hashes to do the set algebra.
		std::vector<State> inBoundStates, outOfBoundStates;

		for (const State &s : currentStates)
		{
			result.nodesExpanded++;

			const int lb = static_cast<int>(bound.HCost(s, goal));
			if (lb == 0)
				return i;                       // final configuration, reached in i moves
			const bool pruned = (lb+i > mu);

			env.GetActions(s, actions);
			for (const auto &a : actions)
			{
				State raw = s;
				env.ApplyAction(raw, a);
				result.nodesGenerated++;

				// Collapse to the class representative before the layer set algebra, so a
				// layer holds one member per equivalence class rather than all of them.
				const State next = canonical(raw);
				const uint64_t key = env.GetStateHash(next);
				if (current.count(key) || previous.count(key))
					continue;                   // one of the two layers we still hold

				if (pruned)
				{
					if (outOfBound.insert(key).second)
						outOfBoundStates.push_back(next);
				}
				else
				{
					if (inBound.insert(key).second)
						inBoundStates.push_back(next);
				}
			}
		}

		// level_{i+1} = inBound \ outOfBound. The knowledge that a configuration is
		// unreachable-in-budget through *some* parent applies to the configuration itself.
		nextStates.clear();
		for (const State &s : inBoundStates)
		{
			if (!outOfBound.count(env.GetStateHash(s)))
				nextStates.push_back(s);
		}

		const uint64_t held = previous.size()+current.size()+inBound.size()+outOfBound.size();
		if (held > result.maxElements)
			result.maxElements = held;

		if (nextStates.empty())
		{
			result.exhausted = true;
			return -1;                          // no solution within mu
		}

		previous = std::move(current);
		current.clear();
		for (const State &s : nextStates)
			current.insert(env.GetStateHash(s));
		currentStates = nextStates;
	}
	return -1;
}

/**
 * Their algorithm as a solver for the optimum: run Algorithm 1 for increasing mu, starting
 * at the root lower bound, and return the first mu that succeeds.
 *
 * Starting at the root bound rather than at 0 skips rounds that cannot possibly succeed,
 * and the first success is optimal because a round with budget mu reports the layer index
 * at which it found a final configuration, and layers are explored in increasing move
 * count. `maxMu` caps the sweep so a runner can time out cleanly rather than loop forever.
 */
template <int numColors, int tubeHeight>
RSCBTResult RSCBTSolve(const BallSort<numColors, tubeHeight> &env,
					   const BallSortState<numColors, tubeHeight> &start,
					   bool symmetryReduction = true, int maxMu = 1000)
{
	RSCBTResult result;
	BallSortDFVSHeuristic<numColors, tubeHeight> bound(&env);
	const auto goal = env.GetGoalState();

	int mu = static_cast<int>(bound.HCost(start, goal));
	if (mu == 0)
	{
		result.solutionLength = 0;
		return result;
	}

	for (; mu <= maxMu; mu++)
	{
		result.iterations++;
		result.exhausted = false;
		int found = RSCBTDecide<numColors, tubeHeight>(env, bound, start, mu,
													  symmetryReduction, result);
		if (found >= 0)
		{
			result.solutionLength = found;
			return result;
		}
	}
	return result;
}

#endif // BALLSORT_RSCBT_H
