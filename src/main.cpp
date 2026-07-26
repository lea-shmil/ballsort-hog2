// Domain smoke test for BallSort.
//
// Checks the invariants the search algorithms rely on -- action legality, apply/undo
// symmetry, and a perfect injective hash -- then cross-checks that BFS, Dijkstra
// (A* with h=0) and A* with the misplaced-ball heuristic all return the same optimal
// solution length. That agreement is the evidence the domain and heuristic are correct.

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "hog2_prelude.h" // must precede all HOG2 headers

#include "SearchEnvironment.h"
#include "BFS.h"
#include "TemplateAStar.h"

#include "BallSort.h"

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

// Run the optimal algorithms and require identical solution lengths.
template <int C, int H>
void CheckOptimalAgreement(BallSort<C, H> &env, const BallSortState<C, H> &start, int instance)
{
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();

	std::vector<State> path;

	BFS<State, BallSortMove, BallSort<C, H>> bfs;
	bfs.SetVerbose(false);
	bfs.GetPath(&env, start, goal, path);
	int bfsLength = static_cast<int>(path.size())-1;
	uint64_t bfsExpanded = bfs.GetNodesExpanded();

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

	printf("  instance %d: optimal=%d  expanded BFS=%llu Dijkstra=%llu A*=%llu\n",
		   instance, bfsLength,
		   (unsigned long long)bfsExpanded,
		   (unsigned long long)dijkstraExpanded,
		   (unsigned long long)astarExpanded);

	Check(bfsLength > 0, "solution found");
	Check(bfsLength == dijkstraLength, "BFS and Dijkstra agree on optimal length");
	Check(bfsLength == astarLength, "BFS and A* agree on optimal length");
	Check(env.HCost(start) <= bfsLength, "heuristic is admissible on this instance");
	Check(astarExpanded <= dijkstraExpanded, "A* expands no more nodes than Dijkstra");
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

	printf("optimal-length agreement, BallSort(3,3)\n");
	{
		BallSort<3, 3> env;
		for (int i = 0; i < 5; i++)
			CheckOptimalAgreement<3, 3>(env, RandomStart<3, 3>(rng), i);
	}

	if (failures == 0)
	{
		printf("OK: all BallSort checks passed\n");
		return 0;
	}
	printf("FAILED: %d check(s)\n", failures);
	return 1;
}
