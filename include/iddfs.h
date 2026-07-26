// iddfs.h -- a from-scratch IDDFS/DFID, used in place of both
// hog2/generic/DFID.h and IDAStar<State, Action> + ZeroHeuristic.
//
// hog2/generic/DFID.h has its goal test commented out in both GetPath overloads
// (grep it for "GoalTest"), so DoIteration always returns false and GetPath loops
// forever raising the bound. IDDFS is, in principle, exactly IDA* with h=0 -- both are
// depth-first, iterative-deepening on g alone -- so IDAStar<State, Action> with a
// ZeroHeuristic looked like a clean substitution. It isn't: IDAStar's top-level success
// check is `DoIteration(...) == 0`, and DoIteration's failure path *also* returns the
// root's own h value when no goal is found within the bound. With h identically 0, that
// failure return is indistinguishable from the success return, so GetPath stops after
// the very first call believing it found the goal when it didn't. This stays invisible
// with a real heuristic (root h is essentially never exactly 0) but fires on every
// instance with h=0, which is exactly IDDFS's use case. Hence: our own, unambiguous
// implementation, not a patch to hog2/.

#ifndef BALLSORT_IDDFS_H
#define BALLSORT_IDDFS_H

#include <cstdint>
#include <vector>

template <class Environment, class State>
struct IDDFSResult {
	int solutionLength;
	uint64_t nodesExpanded;
};

template <class Environment, class State>
bool IDDFSDepthLimited(Environment &env, const State &parent, const State &curr, const State &goal,
					   int limit, uint64_t &nodesExpanded)
{
	if (env.GoalTest(curr, goal))
		return true;
	if (limit == 0)
		return false;

	nodesExpanded++;
	std::vector<State> successors;
	env.GetSuccessors(curr, successors);
	for (const auto &nb : successors)
	{
		if (nb == parent) // skip the immediate back-edge, same as hog2's DFID.h intends
			continue;
		if (IDDFSDepthLimited(env, curr, nb, goal, limit-1, nodesExpanded))
			return true;
	}
	return false;
}

// Runs iterative deepening from bound 0 upward. The domain is finite and every start is
// solvable (see CLAUDE.md), so this always terminates -- there is no "not found" case to
// report, unlike frontier_bfs.h's -1 sentinel.
template <class Environment, class State>
IDDFSResult<Environment, State> IDDFSSolve(Environment &env, const State &start, const State &goal)
{
	uint64_t nodesExpanded = 0;
	for (int limit = 0; ; limit++)
	{
		if (IDDFSDepthLimited(env, start, start, goal, limit, nodesExpanded))
			return {limit, nodesExpanded};
	}
}

#endif // BALLSORT_IDDFS_H
