// frontier_bfs.h -- a from-scratch frontier BFS, used in place of HOG2's
// hog2/generic/FrontierBFS.h.
//
// That upstream header (branch PDB-refactor) never calls GoalTest and never
// populates a path -- GetPath's state-vector overload runs ExpandLevel until
// both open lists are empty (i.e. the entire reachable space) and leaves
// thePath untouched, and the action-vector overload is `assert(!"not
// defined")`. It looks unfinished rather than broken-on-purpose. Since we
// don't touch hog2/, this reimplements the algorithm instead of patching it.
//
// Frontier BFS's point is O(layer width) memory instead of O(states) by
// discarding all but the current and immediately preceding layer. That's
// sound for any unit-cost graph, directed or not: if state x is at BFS depth
// d, every neighbor of x is at depth d-1, d, or d+1 -- a neighbor at depth
// d-2 would make x's own depth d-1, a contradiction. So checking only the
// current and previous layers is enough to avoid revisiting a state, without
// keeping the full closed set. What it does not do is reconstruct a path;
// that needs the divide-and-conquer trick the classic algorithm uses, which
// this project has no need for -- the correctness contract only needs the
// solution length.

#ifndef BALLSORT_FRONTIER_BFS_H
#define BALLSORT_FRONTIER_BFS_H

#include <cstdint>
#include <unordered_map>
#include <vector>

template <class Environment, class State>
struct FrontierBFSResult {
	int solutionLength; // -1 if the goal was never generated
	uint64_t nodesExpanded;
};

template <class Environment, class State>
FrontierBFSResult<Environment, State> FrontierBFSSolve(Environment &env, const State &start, const State &goal)
{
	if (env.GoalTest(start, goal))
		return {0, 0};

	std::unordered_map<uint64_t, bool> closedPrevious; // depth d-1
	std::vector<State> currentLayer{start};
	std::vector<State> successors;
	uint64_t nodesExpanded = 0;
	int depth = 0;

	while (!currentLayer.empty())
	{
		std::unordered_map<uint64_t, bool> nextSeen; // dedup within the layer being built
		std::vector<State> nextLayer;

		for (const auto &s : currentLayer)
		{
			nodesExpanded++;
			env.GetSuccessors(s, successors);
			for (const auto &nb : successors)
			{
				uint64_t h = env.GetStateHash(nb);
				if (closedPrevious.find(h) != closedPrevious.end())
					continue; // don't step back into the layer we came from
				if (nextSeen.find(h) != nextSeen.end())
					continue; // a sibling already reached this state this round
				nextSeen[h] = true;

				if (env.GoalTest(nb, goal))
					return {depth+1, nodesExpanded};

				nextLayer.push_back(nb);
			}
		}

		closedPrevious.clear();
		for (const auto &s : currentLayer)
			closedPrevious[env.GetStateHash(s)] = true;

		currentLayer.swap(nextLayer);
		depth++;
	}

	return {-1, nodesExpanded};
}

#endif // BALLSORT_FRONTIER_BFS_H
