// Domain smoke test for BallSort.
//
// Checks the invariants the search algorithms rely on -- action legality, apply/undo
// symmetry, and a perfect injective hash -- then runs the best-first search family from
// the project brief and cross-checks that every optimal algorithm (BFS, bidirectional
// BFS, frontier BFS, IDDFS/DFID, Dijkstra, A*, IDA*) returns the same optimal solution
// length. That agreement is the evidence the domain and heuristic are correct. Weighted
// A* and greedy best-first are exempt from that requirement by design and are checked
// against their own bounds instead.
//
// Two algorithms are our own implementation rather than the HOG2 header the brief names,
// both because we don't touch hog2/ (see README) and both upstream bugs, not just gaps:
//
//  - IDDFS/DFID uses include/iddfs.h. hog2/generic/DFID.h has its goal test commented
//    out in both GetPath overloads (grep it for "GoalTest"), so DoIteration always
//    returns false and GetPath loops forever raising the bound. IDDFS is, in principle,
//    exactly IDA* with h=0, so IDAStar<State, Action> + ZeroHeuristic looked like a
//    clean substitute -- except IDAStar's own top-level success check is
//    `DoIteration(...) == 0`, and DoIteration's *failure* path also returns the root's
//    h value, so with h identically 0 the two cases are indistinguishable and GetPath
//    stops after the first call believing it found the goal. That's invisible with our
//    real heuristic (see the note at the IDA* call site below for why), but fires on
//    every instance with h=0 -- exactly IDDFS's case. Two independent upstream defects,
//    hence our own unambiguous implementation for this one.
//  - Frontier BFS uses include/frontier_bfs.h. hog2/generic/FrontierBFS.h never calls
//    GoalTest and its GetPath never populates thePath; it only runs to full exhaustion
//    of the reachable state space. See the comment in frontier_bfs.h for why checking
//    just the current and previous BFS layers is sufficient to reimplement it correctly.
//
// IDDFS/DFID is also excluded from the BallSort(3,3) comparison run below and checked
// separately on BallSort(3,2). With no heuristic and a high branching factor (no color
// matching, so up to numTubes*(numTubes-1) moves per state), plain iterative deepening
// on (3,3) cost 161M node expansions and 52s wall-clock for a single instance in
// testing -- exactly the kind of work CLAUDE.md reserves for compute nodes via
// sbatch/srun, not this login-node smoke test. IDA* with the real heuristic largely
// avoids the same blowup (see its call site), which is itself a relevant data point.

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "hog2_prelude.h" // must precede all HOG2 headers

#include "SearchEnvironment.h"
#include "BFS.h"
#include "UnitCostBidirectionalBFS.h"
#include "IDAStar.h"
#include "TemplateAStar.h"

#include "BallSort.h"
#include "frontier_bfs.h"
#include "iddfs.h"

namespace {

int failures = 0;

void Check(bool condition, const char *what)
{
	if (!condition)
	{
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

// A random full-tube start: every colored tube full, reserve empty. Per the brief,
// any such arrangement with exactly tubeHeight balls per color is solvable, so there
// is no solvability check to do here.
template <int C, int H>
BallSortState<C, H> RandomStart(std::mt19937 &rng)
{
	std::vector<int> colors;
	for (int c = 1; c <= C; c++)
		for (int k = 0; k < H; k++)
			colors.push_back(c);
	std::shuffle(colors.begin(), colors.end(), rng);

	BallSortState<C, H> s;
	s.SetFromColorSequence(colors);
	return s;
}

// Exhaustively explore the component reachable from `start`, verifying as we go that
// the hash is injective and that GetStateFromHash inverts it.
template <int C, int H>
void CheckDomainInvariants(BallSort<C, H> &env, const BallSortState<C, H> &start)
{
	typedef BallSortState<C, H> State;

	std::unordered_map<uint64_t, State> byHash;
	std::vector<State> open;
	std::unordered_map<State, bool> seen;

	open.push_back(start);
	seen[start] = true;

	std::vector<State> successors;
	std::vector<BallSortMove> actions;
	size_t transitions = 0;

	while (!open.empty())
	{
		State s = open.back();
		open.pop_back();

		// Hash must be injective, in range, and invertible.
		uint64_t h = env.GetStateHash(s);
		Check(h < env.GetMaxHash(), "state hash within GetMaxHash()");

		auto it = byHash.find(h);
		if (it == byHash.end())
			byHash[h] = s;
		else
			Check(it->second == s, "hash collision between distinct states");

		State decoded;
		env.GetStateFromHash(h, decoded);
		Check(decoded == s, "GetStateFromHash inverts GetStateHash");

		// Ball conservation: H balls of every color, always.
		int perColor[C+1] = {0};
		for (int t = 0; t <= C; t++)
			for (int k = 0; k < s.GetBallCountInTube(t); k++)
				perColor[s.GetBallInTube(t, k)]++;
		for (int c = 1; c <= C; c++)
			Check(perColor[c] == H, "ball count per color preserved");

		env.GetActions(s, actions);
		for (auto a : actions)
		{
			Check(!s.IsEmpty(a.source), "action source non-empty");
			Check(!s.IsFull(a.dest), "action dest not full");
			Check(a.source != a.dest, "action source != dest");

			State next = s;
			env.ApplyAction(next, a);
			transitions++;

			// Undo must restore the state exactly.
			State restored = next;
			env.UndoAction(restored, a);
			Check(restored == s, "UndoAction inverts ApplyAction");

			// The heuristic must never overestimate a single step's effect.
			Check(std::abs(env.HCost(next) - env.HCost(s)) <= 1.0,
				  "heuristic changes by at most the move cost (consistency)");

			if (seen.find(next) == seen.end())
			{
				seen[next] = true;
				open.push_back(next);
			}
		}
	}

	printf("  reachable states: %zu, transitions: %zu\n", seen.size(), transitions);

	State goal = env.GetGoalState();
	Check(env.GoalTest(goal), "goal state passes GoalTest");
	Check(env.HCost(goal) == 0, "heuristic is 0 at the goal");
	Check(seen.find(goal) != seen.end(), "goal is reachable from the start");
}

// Run every optimal algorithm from the brief and require identical solution lengths.
// Returns the optimal length, for CheckWeightedAndGreedy's ratio bookkeeping.
template <int C, int H>
int CheckOptimalAgreement(BallSort<C, H> &env, const BallSortState<C, H> &start, int instance)
{
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();

	std::vector<State> path;

	BFS<State, BallSortMove, BallSort<C, H>> bfs;
	bfs.SetVerbose(false);
	bfs.GetPath(&env, start, goal, path);
	int bfsLength = static_cast<int>(path.size())-1;
	uint64_t bfsExpanded = bfs.GetNodesExpanded();

	UnitCostBidirectionalBFS<State, BallSortMove> biBfs;
	biBfs.GetPath(&env, start, goal, path);
	int biBfsLength = static_cast<int>(path.size())-1;
	uint64_t biBfsExpanded = biBfs.GetNodesExpanded();

	auto frontier = FrontierBFSSolve(env, start, goal);
	int frontierLength = frontier.solutionLength;
	uint64_t frontierExpanded = frontier.nodesExpanded;

	ZeroHeuristic<State> zero;
	TemplateAStar<State, BallSortMove, BallSort<C, H>> dijkstra;
	dijkstra.SetHeuristic(&zero);
	dijkstra.GetPath(&env, start, goal, path);
	int dijkstraLength = static_cast<int>(path.size())-1;
	uint64_t dijkstraExpanded = dijkstra.GetNodesExpanded();

	TemplateAStar<State, BallSortMove, BallSort<C, H>> astar;
	astar.GetPath(&env, start, goal, path);
	int astarLength = static_cast<int>(path.size())-1;
	uint64_t astarExpanded = astar.GetNodesExpanded();

	// IDA* with the real heuristic. Neither SetHeuristic call needed: with none set,
	// IDAStar falls back to the environment itself (SearchEnvironment derives from
	// Heuristic<state>), which is BallSort::HCost. Immune to the root-h-zero bug
	// described at the top of this file: our HCost is 0 only at the goal (it counts
	// balls out of place, and GoalTest is exactly "zero balls out of place"), so the
	// success and failure return paths can only collide when start already equals goal.
	IDAStar<State, BallSortMove, false> idaStar;
	idaStar.GetPath(&env, start, goal, path);
	int idaStarLength = static_cast<int>(path.size())-1;
	uint64_t idaStarExpanded = idaStar.GetNodesExpanded();

	printf("  instance %d: optimal=%d\n", instance, bfsLength);
	printf("    expanded: BFS=%llu biBFS=%llu frontierBFS=%llu Dijkstra=%llu A*=%llu IDA*=%llu\n",
		   (unsigned long long)bfsExpanded, (unsigned long long)biBfsExpanded,
		   (unsigned long long)frontierExpanded,
		   (unsigned long long)dijkstraExpanded, (unsigned long long)astarExpanded,
		   (unsigned long long)idaStarExpanded);

	Check(bfsLength > 0, "solution found");
	Check(bfsLength == biBfsLength, "BFS and bidirectional BFS agree on optimal length");
	Check(bfsLength == frontierLength, "BFS and frontier BFS agree on optimal length");
	Check(bfsLength == dijkstraLength, "BFS and Dijkstra agree on optimal length");
	Check(bfsLength == astarLength, "BFS and A* agree on optimal length");
	Check(bfsLength == idaStarLength, "BFS and IDA* agree on optimal length");
	Check(env.HCost(start) <= bfsLength, "heuristic is admissible on this instance");
	Check(astarExpanded <= dijkstraExpanded, "A* expands no more nodes than Dijkstra");

	return bfsLength;
}

// IDDFS/DFID, checked separately on the small domain -- see the file header for why it
// doesn't belong in CheckOptimalAgreement's BallSort(3,3) run.
template <int C, int H>
void CheckIDDFSAgreement(BallSort<C, H> &env, const BallSortState<C, H> &start, int instance)
{
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();
	std::vector<State> path;

	BFS<State, BallSortMove, BallSort<C, H>> bfs;
	bfs.SetVerbose(false);
	bfs.GetPath(&env, start, goal, path);
	int bfsLength = static_cast<int>(path.size())-1;

	auto iddfs = IDDFSSolve(env, start, goal);

	printf("  instance %d: optimal=%d  IDDFS length=%d expanded=%llu\n",
		   instance, bfsLength, iddfs.solutionLength,
		   (unsigned long long)iddfs.nodesExpanded);

	Check(bfsLength > 0, "solution found");
	Check(bfsLength == iddfs.solutionLength, "BFS and IDDFS agree on optimal length");
}

// Weighted A* and greedy best-first are exempt from the equal-optimal-length
// requirement -- they are expected to return solutions of length >= optimal. Record
// their lengths against the optimal one and check the bound each algorithm promises.
template <int C, int H>
void CheckWeightedAndGreedy(BallSort<C, H> &env, const BallSortState<C, H> &start, int instance, int optimalLength)
{
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();
	std::vector<State> path;

	printf("  instance %d (optimal=%d):\n", instance, optimalLength);

	static const double weights[] = {1.5, 2.0, 3.0, 5.0};
	for (double w : weights)
	{
		TemplateAStar<State, BallSortMove, BallSort<C, H>> weighted;
		weighted.SetWeight(w);
		weighted.GetPath(&env, start, goal, path);
		int length = static_cast<int>(path.size())-1;
		double ratio = static_cast<double>(length)/optimalLength;

		printf("    weighted A* w=%.1f: length=%d (ratio=%.3f, expanded=%llu)\n",
			   w, length, ratio, (unsigned long long)weighted.GetNodesExpanded());

		Check(length >= optimalLength, "weighted A* length >= optimal");
		Check(length <= w*optimalLength+1e-9, "weighted A* respects its w * optimal bound");
	}

	// Greedy best-first: order purely on h, ignoring g -- the w = infinity end of the sweep.
	TemplateAStar<State, BallSortMove, BallSort<C, H>> greedy;
	greedy.SetPhi([](double h, double g) { return h; });
	greedy.GetPath(&env, start, goal, path);
	int greedyLength = static_cast<int>(path.size())-1;
	double greedyRatio = static_cast<double>(greedyLength)/optimalLength;

	printf("    greedy best-first: length=%d (ratio=%.3f, expanded=%llu)\n",
		   greedyLength, greedyRatio, (unsigned long long)greedy.GetNodesExpanded());

	Check(greedyLength >= optimalLength, "greedy best-first length >= optimal");
}

} // namespace

int main()
{
	std::mt19937 rng(20250726);

	printf("BallSort(3,2) domain invariants\n");
	{
		BallSort<3, 2> env;
		printf("  %s, maxHash=%llu\n", env.GetName().c_str(),
			   (unsigned long long)env.GetMaxHash());
		CheckDomainInvariants<3, 2>(env, RandomStart<3, 2>(rng));
	}

	printf("BallSort(3,3) domain invariants\n");
	{
		BallSort<3, 3> env;
		printf("  %s, maxHash=%llu\n", env.GetName().c_str(),
			   (unsigned long long)env.GetMaxHash());
		CheckDomainInvariants<3, 3>(env, RandomStart<3, 3>(rng));
	}

	printf("IDDFS/DFID agreement, BallSort(3,2)\n");
	{
		BallSort<3, 2> env;
		for (int i = 0; i < 5; i++)
			CheckIDDFSAgreement<3, 2>(env, RandomStart<3, 2>(rng), i);
	}

	printf("optimal-length agreement, BallSort(3,3)\n");
	{
		BallSort<3, 3> env;
		for (int i = 0; i < 5; i++)
			CheckOptimalAgreement<3, 3>(env, RandomStart<3, 3>(rng), i);
	}

	printf("weighted A* / greedy best-first, BallSort(3,3)\n");
	{
		BallSort<3, 3> env;
		for (int i = 0; i < 3; i++)
		{
			BallSortState<3, 3> start = RandomStart<3, 3>(rng);
			int optimalLength = CheckOptimalAgreement<3, 3>(env, start, i);
			CheckWeightedAndGreedy<3, 3>(env, start, i, optimalLength);
		}
	}

	if (failures == 0)
	{
		printf("OK: all BallSort checks passed\n");
		return 0;
	}
	printf("FAILED: %d check(s)\n", failures);
	return 1;
}
