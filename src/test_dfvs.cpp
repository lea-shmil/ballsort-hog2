// Validation for the DFVS lower bound (include/dfvs_bound.h) and the exact solver it uses
// (include/dfvs_exact.h).
//
// Three independent checks, because the bound is the one piece of this project we ported
// from someone else's implementation and a silently wrong lower bound would make A* and
// IDA* return non-optimal answers while looking perfectly healthy:
//
//  1. The solver against brute force. For graphs small enough to enumerate every vertex
//     subset, the exact minimum DFVS is checkable directly. Random graphs, plus the
//     degenerate shapes (empty, complete, self-loops only) that reduction rules get wrong.
//  2. The bound against ground truth. BFS gives the true optimal move count, so the bound
//     must never exceed it. Run over every instance of the small cells.
//  3. The bound against BallSort::HCost and the paper's simple bound. Their §5 reports the
//     DFVS bound dominating the simple one; if our port comes out weaker, the port is wrong.
//     Also checks consistency, |h(n) - h(n')| <= 1, which A* needs to stay optimal without
//     reopening.

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "hog2_prelude.h" // must precede all HOG2 headers

#include "SearchEnvironment.h"
#include "BFS.h"
#include "TemplateAStar.h"

#include "BallSort.h"
#include "dfvs_bound.h"
#include "dfvs_exact.h"
#include "instance_gen.h"

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

// ---------------------------------------------------------------------------
// 1. exact solver vs. brute force
// ---------------------------------------------------------------------------

// Is the subgraph of `g` induced on `alive` acyclic? Kahn's algorithm on the live vertices.
bool InducedIsAcyclic(const DFVSGraph &g, uint64_t alive)
{
	std::vector<int> indegree(g.n, 0);
	for (int v = 0; v < g.n; v++)
	{
		if (!((alive>>v)&1ull))
			continue;
		for (int w = 0; w < g.n; w++)
			if (((alive>>w)&1ull) && g.HasEdge(v, w))
				indegree[w]++;
	}

	std::vector<int> queue;
	for (int v = 0; v < g.n; v++)
		if (((alive>>v)&1ull) && indegree[v] == 0)
			queue.push_back(v);

	int removed = 0;
	for (size_t qi = 0; qi < queue.size(); qi++)
	{
		int v = queue[qi];
		removed++;
		for (int w = 0; w < g.n; w++)
		{
			if (((alive>>w)&1ull) && g.HasEdge(v, w))
			{
				if (--indegree[w] == 0)
					queue.push_back(w);
			}
		}
	}

	int liveCount = __builtin_popcountll(alive);
	return removed == liveCount;
}

int BruteForceMinDFVS(const DFVSGraph &g)
{
	const uint64_t full = (g.n == 64) ? ~0ull : ((1ull<<g.n)-1);
	int best = g.n;
	for (uint64_t remove = 0; remove <= full; remove++)
	{
		int size = __builtin_popcountll(remove);
		if (size >= best)
			continue;
		if (InducedIsAcyclic(g, full&~remove))
			best = size;
	}
	return best;
}

void CheckSolverAgainstBruteForce()
{
	std::mt19937 rng(90210);
	int cases = 0;

	// Random graphs across a range of densities, including self-loops.
	for (int n = 1; n <= 9; n++)
	{
		for (int density = 1; density <= 6; density++)
		{
			for (int trial = 0; trial < 12; trial++)
			{
				DFVSGraph g(n);
				std::uniform_int_distribution<int> coin(1, 12);
				for (int v = 0; v < n; v++)
					for (int w = 0; w < n; w++)
						if (coin(rng) <= density)
							g.AddEdge(v, w);

				int exact = MinimumDFVSSize(g);
				int brute = BruteForceMinDFVS(g);
				if (exact != brute)
				{
					printf("  FAIL: n=%d density=%d trial=%d: exact=%d brute=%d\n",
						   n, density, trial, exact, brute);
					failures++;
					return;
				}

				// The returned *set* must be both minimum-sized and actually a DFVS.
				std::vector<int> chosen = MinimumDFVSSet(g);
				uint64_t mask = 0;
				for (int v : chosen)
					mask |= (1ull<<v);
				const uint64_t full = (1ull<<n)-1;
				Check(static_cast<int>(chosen.size()) == exact,
					  "MinimumDFVSSet returns a minimum-cardinality set");
				Check(InducedIsAcyclic(g, full&~mask),
					  "MinimumDFVSSet actually breaks every cycle");
				cases++;
			}
		}
	}

	// Degenerate shapes that reduction rules are most likely to mishandle.
	{
		DFVSGraph empty(6);
		Check(MinimumDFVSSize(empty) == 0, "empty graph needs no vertices removed");

		DFVSGraph loops(5);
		for (int v = 0; v < 5; v++)
			loops.AddEdge(v, v);
		Check(MinimumDFVSSize(loops) == 5, "every self-loop must be removed");

		DFVSGraph complete(6);
		for (int v = 0; v < 6; v++)
			for (int w = 0; w < 6; w++)
				if (v != w)
					complete.AddEdge(v, w);
		// A tournament-free complete digraph on n vertices: only one vertex can survive.
		Check(MinimumDFVSSize(complete) == 5, "complete digraph leaves one vertex");
		Check(BruteForceMinDFVS(complete) == 5, "brute force agrees on the complete digraph");

		DFVSGraph cycle(7);
		for (int v = 0; v < 7; v++)
			cycle.AddEdge(v, (v+1)%7);
		Check(MinimumDFVSSize(cycle) == 1, "a single directed cycle needs one vertex");

		DFVSGraph twoCycles(8);
		for (int v = 0; v < 4; v++)
			twoCycles.AddEdge(v, (v+1)%4);
		for (int v = 4; v < 8; v++)
			twoCycles.AddEdge(v, 4+((v-4+1)%4));
		Check(MinimumDFVSSize(twoCycles) == 2, "disjoint cycles need one vertex each");
	}

	printf("  %d random graphs cross-checked against brute force\n", cases);
}

// ---------------------------------------------------------------------------
// 2 & 3. the bound vs. ground truth, and vs. the weaker bounds
// ---------------------------------------------------------------------------

template <int C, int H>
void CheckBoundOnCell(int numTubes, int count)
{
	BallSort<C, H> env;
	BallSortDFVSHeuristic<C, H> dfvs(&env);
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();

	int dominatedSimple = 0, strictlyBetter = 0;
	long long totalDFVS = 0, totalSimple = 0, totalOptimal = 0;

	for (int i = 0; i < count; i++)
	{
		std::mt19937 gen(static_cast<std::mt19937::result_type>(InstanceSeed(20250726, H, numTubes, i)));
		std::vector<int> colors = RandomColorSequence(C, H, gen);
		State start;
		start.SetFromColorSequence(colors);

		std::vector<State> path;
		BFS<State, BallSortMove, BallSort<C, H>> bfs;
		bfs.SetVerbose(false);
		bfs.GetPath(&env, start, goal, path);
		int optimal = static_cast<int>(path.size())-1;

		int paperBound = static_cast<int>(dfvs.HCost(start, goal));
		int dfvsTerm = DFVSLowerBound<C, H>(start);
		int basic = BasicLowerBound<C, H>(start);
		int simple = SimpleLowerBound(colors, C, H);
		int misplaced = static_cast<int>(env.HCost(start));

		// The state-based basic bound must agree with the start-sequence version in
		// instance_gen.h; a start state has an empty reserve, so the two see the same balls.
		Check(basic == simple, "BasicLowerBound agrees with SimpleLowerBound on a start state");
		// The fast DFVS term must equal the literal transcription of their formula.
		Check(dfvsTerm == DFVSLowerBoundViaSet<C, H>(start),
			  "fast DFVS term equals their set-based formula");
		Check(paperBound == basic+dfvsTerm, "paper bound is basic + DFVS term");
		Check(paperBound <= optimal, "paper's DFVS lower bound is admissible");
		Check(dfvsTerm >= 0, "DFVS term is non-negative");
		if (paperBound >= simple)
			dominatedSimple++;
		if (paperBound > simple)
			strictlyBetter++;
		totalDFVS += paperBound;
		totalSimple += simple;
		totalOptimal += optimal;

		printf("    instance %2d: optimal=%2d  paper=%2d (basic=%2d + dfvs=%d)  simple=%2d  HCost=%2d\n",
			   i, optimal, paperBound, basic, dfvsTerm, simple, misplaced);
	}

	printf("  cell %dx%d: paper >= simple on %d/%d, strictly better on %d/%d; "
		   "mean paper=%.2f simple=%.2f optimal=%.2f\n",
		   H, numTubes, dominatedSimple, count, strictlyBetter, count,
		   static_cast<double>(totalDFVS)/count,
		   static_cast<double>(totalSimple)/count,
		   static_cast<double>(totalOptimal)/count);
	Check(dominatedSimple == count, "paper's bound dominates the simple bound (their §5 claim)");
}

// Consistency over the whole reachable set of a small cell: A* needs |h(n)-h(n')| <= 1 for
// unit-cost moves, or it can expand a node before its g is settled.
template <int C, int H>
void CheckBoundConsistency()
{
	BallSort<C, H> env;
	BallSortDFVSHeuristic<C, H> dfvs(&env);
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();

	std::vector<State> open;
	std::unordered_map<State, bool> seen;
	State start;
	{
		std::mt19937 gen(1234);
		start.SetFromColorSequence(RandomColorSequence(C, H, gen));
	}
	open.push_back(start);
	seen[start] = true;

	std::vector<BallSortMove> actions;
	size_t states = 0;
	int worstJump = 0;

	while (!open.empty())
	{
		State s = open.back();
		open.pop_back();
		states++;

		double hs = dfvs.HCost(s, goal);
		if (env.GoalTest(s))
			Check(hs == 0, "paper bound is zero at the goal");
		else
			Check(hs > 0, "paper bound is positive away from the goal");

		env.GetActions(s, actions);
		for (auto a : actions)
		{
			State next = s;
			env.ApplyAction(next, a);
			double hn = dfvs.HCost(next, goal);
			int jump = static_cast<int>(std::abs(hn-hs));
			if (jump > worstJump)
				worstJump = jump;
			if (seen.find(next) == seen.end())
			{
				seen[next] = true;
				open.push_back(next);
			}
		}
	}

	// Measured, not asserted. Admissibility is what A* needs for optimality; consistency is
	// what lets it skip reopening closed nodes. A bound of the form
	// "per-ball count + minimum DFVS" has no obvious reason to be consistent, and if it is
	// not, TemplateAStar has to be run with node reopening enabled or it can close a node
	// before its g settles and return a suboptimal path. Whatever this prints is the fact
	// the runner has to respect, so print it rather than guess.
	printf("  over %zu reachable states: largest |h(n)-h(n')| = %d  (%s)\n",
		   states, worstJump,
		   worstJump <= 1 ? "consistent" : "INCONSISTENT -- A* needs reopening");
}

// The bound is only useful if A* using it still returns the optimal length. This is the
// end-to-end check that matters, and it is the correctness contract from CLAUDE.md.
template <int C, int H>
void CheckAStarOptimalityWithBound(int numTubes, int count)
{
	BallSort<C, H> env;
	BallSortDFVSHeuristic<C, H> dfvs(&env);
	typedef BallSortState<C, H> State;
	State goal = env.GetGoalState();

	for (int i = 0; i < count; i++)
	{
		std::mt19937 gen(static_cast<std::mt19937::result_type>(InstanceSeed(20250726, H, numTubes, i)));
		State start;
		start.SetFromColorSequence(RandomColorSequence(C, H, gen));

		std::vector<State> path;
		BFS<State, BallSortMove, BallSort<C, H>> bfs;
		bfs.SetVerbose(false);
		bfs.GetPath(&env, start, goal, path);
		int optimal = static_cast<int>(path.size())-1;

		TemplateAStar<State, BallSortMove, BallSort<C, H>> astar;
		astar.SetHeuristic(&dfvs);
		astar.SetReopenNodes(true);      // safe under an inconsistent admissible heuristic
		astar.GetPath(&env, start, goal, path);
		int astarLength = static_cast<int>(path.size())-1;

		TemplateAStar<State, BallSortMove, BallSort<C, H>> plain;
		plain.GetPath(&env, start, goal, path);

		printf("    instance %2d: optimal=%2d  A*+paperBound=%2d  expanded %llu vs %llu with HCost\n",
			   i, optimal, astarLength,
			   (unsigned long long)astar.GetNodesExpanded(),
			   (unsigned long long)plain.GetNodesExpanded());
		Check(astarLength == optimal, "A* with the paper's bound returns the optimal length");
	}
}

} // namespace

int main()
{
	// Unbuffered: this runs under srun/sbatch with stdout to a file, and the consistency
	// sweeps are slow enough that block buffering would hide all progress until the end.
	setvbuf(stdout, nullptr, _IONBF, 0);

	printf("exact DFVS solver vs. brute force\n");
	CheckSolverAgainstBruteForce();

	printf("DFVS bound vs. optimal, cell 3x4 (c=3, h=3)\n");
	CheckBoundOnCell<3, 3>(4, 10);

	printf("DFVS bound vs. optimal, cell 4x3 (c=2, h=4)\n");
	CheckBoundOnCell<2, 4>(3, 10);

	printf("paper bound consistency, cell 2x4 (c=3, h=2)\n");
	CheckBoundConsistency<3, 2>();

	printf("paper bound consistency, cell 3x4 (c=3, h=3)\n");
	CheckBoundConsistency<3, 3>();

	printf("A* optimality with the paper's bound, cell 3x4 (c=3, h=3)\n");
	CheckAStarOptimalityWithBound<3, 3>(4, 10);

	printf("A* optimality with the paper's bound, cell 4x3 (c=2, h=4)\n");
	CheckAStarOptimalityWithBound<2, 4>(3, 10);

	if (failures == 0)
	{
		printf("OK: all DFVS checks passed\n");
		return 0;
	}
	printf("FAILED: %d check(s)\n", failures);
	return 1;
}
