// dfvs_bound.h -- the DFVS-based lower bound of Althaus et al., as a HOG2 heuristic.
//
// ATTRIBUTION. This bound is theirs, not ours. It is the "DFVS-based lower bound" of
// Althaus, E.; Blumenstock, M.; Rassau, N.; Schuhknecht, F. M.; and Zimdars, A. Q. 2025,
// "Sorting colored balls in colored tubes", SoCS 2025, 11-19 -- the one their §5 reports
// dominating their simple and lookup-based bounds, and the one that got them to 7 colored
// tubes of height 4. See the README's provenance section.
//
// The graph construction and the bound formula below are a faithful port of
// `tube_rack_data_to_graph_instance` and `dfvs_lower_bound` in `src/dfvs_interface.cpp` of
// their repository, reimplemented in our own code and against our own state type. It is
// worth being explicit that this follows their *implementation* rather than the paper's
// exposition, because the two differ in an important way:
//
//   The paper motivates the bound via the Feedback Arc Set problem on a graph with one
//   vertex per color (Construction 6 / Proposition 7), then notes they solve it by reducing
//   FAS to DFVS. Their code does not build that per-color graph. It builds a graph with
//   **one vertex per tube slot** -- n = numTubes*tubeHeight, empty slots included as
//   isolated vertices -- and solves DFVS on that directly. The two are not the same graph,
//   and the per-slot version is what produced their published numbers, so it is what we
//   implement.
//
// We do not vendor their solver. Their `LOWER-BOUND` calls the PACE 2022 exact-track solver
// "rubengoetz", vendored into their (unlicensed) repository; ours calls dfvs_exact.h, which
// is our own exact branch and bound. Both compute an exact minimum, so the bound values
// agree exactly; only the runtime differs.

#ifndef BALLSORT_DFVS_BOUND_H
#define BALLSORT_DFVS_BOUND_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Heuristic.h"

#include "BallSort.h"
#include "dfvs_exact.h"

/**
 * Build their per-slot graph from a configuration.
 *
 * Vertex numbering is ours (slot k of tube t is vertex t*tubeHeight+k, with tube 0 the
 * reserve); theirs puts the reserve last. Numbering does not affect a minimum DFVS, and
 * the edge structure below follows theirs case for case.
 *
 * Edges, for the ball at vertex `v` sitting in tube `t`:
 *
 *  - ball is in final position (its color is t, and every ball below it in t is also t):
 *    no edges. It never has to move.
 *  - ball's color is t but the tube is *not* still all-t below it: edges to every ball
 *    that has to leave tube t, which includes this ball itself. That **self-loop is
 *    deliberate** -- their code carries a comment recording that removing it (their
 *    commented-out variant) produced a weaker bound on cell 6x4, 26 instead of 27, so the
 *    loop stays and the bound formula subtracts these vertices back out afterwards.
 *  - ball is in the wrong tube (including anything in the reserve): edges to every ball
 *    that has to leave the ball's *home* tube, plus edges to every ball stacked above it
 *    in its current tube -- those have to come off before this one can move.
 *
 * `mustMove[color]` is the set of balls that have to leave tube `color`: everything from
 * the first foreign ball upward. Balls below that are already in final position.
 */
template <int numColors, int tubeHeight>
DFVSGraph BuildDFVSGraph(const BallSortState<numColors, tubeHeight> &s)
{
	const int numTubes = numColors+1;
	const int numSlots = numTubes*tubeHeight;

	auto vertexOf = [](int tube, int slot) { return tube*tubeHeight+slot; };

	// Pass 1: which balls have to leave their own colored tube.
	std::vector<std::vector<int>> mustMove(numColors+1);
	for (int t = 1; t <= numColors; t++)
	{
		bool foundForeign = false;
		for (int k = 0; k < tubeHeight; k++)
		{
			int color = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;
			if (color == 0)
				break;              // packed from the bottom, so nothing above matters
			if (color == t)
			{
				if (!foundForeign)
					continue;       // still in final position
				mustMove[t].push_back(vertexOf(t, k));
			}
			else
			{
				foundForeign = true;
				mustMove[t].push_back(vertexOf(t, k));
			}
		}
	}

	// Pass 2: the edges.
	DFVSGraph g(numSlots);
	for (int t = 0; t < numTubes; t++)
	{
		const int tubeColor = t;    // tube 0 is the reserve and matches no ball color
		bool tubeOptimal = true;
		for (int k = 0; k < tubeHeight; k++)
		{
			const int v = vertexOf(t, k);
			const int color = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;

			if (color == 0)
			{
				tubeOptimal = false;    // isolated vertex, no edges
			}
			else if (color == tubeColor)
			{
				if (!tubeOptimal)
				{
					for (int target : mustMove[color])
						g.AddEdge(v, target);   // includes v itself: the deliberate self-loop
				}
			}
			else
			{
				tubeOptimal = false;
				for (int target : mustMove[color])
					g.AddEdge(v, target);
				for (int above = k+1; above < tubeHeight; above++)
				{
					int aboveColor = (above < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, above) : 0;
					if (aboveColor == 0)
						break;
					g.AddEdge(v, vertexOf(t, above));
				}
			}
		}
	}
	return g;
}

/**
 * Their per-tube "basic" bound, generalized to any configuration.
 *
 * This is `tube::lower_bound_for_needed_operations` summed over the tubes, i.e. their
 * `TubeRack::basic_lower_bound_for_needed_operations`, and it is the same 0/1/2 rule as
 * SimpleLowerBound in instance_gen.h -- except that one is written against a flat start
 * sequence, while the search needs arbitrary states, reserve included:
 *
 *   colored tube t, scanning from the bottom, stopping at the first empty slot:
 *     once any ball has failed to be color t, every further ball costs
 *     1 + (ball is color t), i.e. 2 for a home-color ball stuck above a foreign one
 *     and 1 for a foreign ball. Balls in the all-t prefix cost 0.
 *   reserve: 1 per ball, since the goal requires it empty.
 */
template <int numColors, int tubeHeight>
int BasicLowerBound(const BallSortState<numColors, tubeHeight> &s)
{
	int bound = 0;
	for (int k = 0; k < s.GetBallCountInTube(0); k++)
		bound++;                    // every ball in the reserve has to leave it
	for (int t = 1; t <= numColors; t++)
	{
		bool optimal = true;
		for (int k = 0; k < tubeHeight; k++)
		{
			int color = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;
			if (color == 0)
				break;
			bool sameColor = (color == t);
			optimal = optimal && sameColor;
			if (!optimal)
				bound += 1+(sameColor ? 1 : 0);
		}
	}
	return bound;
}

/**
 * The DFVS term: the size of a minimum DFVS of the graph above, minus the DFVS members that
 * are home-tube balls out of final position.
 *
 * The subtraction is theirs and is what the deliberate self-loops pay for: those vertices
 * are forced into every minimum DFVS by their loops, and are then removed from the count.
 * This term is always non-negative, since every subtracted vertex has a self-loop and so
 * lies in every minimum DFVS.
 *
 * On its own this is *not* their lower bound -- see PaperLowerBound below. Their
 * `dfvs_lower_bound` is only this term; `TubeRack::lower_bound_for_needed_operations`, the
 * function the search actually calls, is `basic_lower_bound + dfvs_lower_bound`. That
 * matches their Proposition 7, where a feedback arc set of size k corresponds to |E|+k
 * moves: the basic bound is the |E| part and this term is the k.
 */
template <int numColors, int tubeHeight>
int DFVSLowerBound(const BallSortState<numColors, tubeHeight> &s)
{
	const int numSlots = (numColors+1)*tubeHeight;
	if (numSlots > 64)
		return 0;   // bitmask solver limit; no cell in the paper's grid exceeds 42

	// Home-tube balls that are not in final position, exactly as their loop finds them.
	std::vector<int> notOptimalHome;
	for (int t = 1; t <= numColors; t++)
	{
		bool tubeOptimal = true;
		for (int k = 0; k < tubeHeight; k++)
		{
			int color = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;
			if (color != t)
				tubeOptimal = false;
			else if (!tubeOptimal)
				notOptimalHome.push_back(t*tubeHeight+k);
		}
	}

	DFVSGraph g = BuildDFVSGraph<numColors, tubeHeight>(s);

	// Their code computes a minimum DFVS *set* and subtracts the notOptimalHome members it
	// contains. We only need the size, because every notOptimalHome vertex is in *every*
	// minimum DFVS: such a ball has color t, sits in tube t above a foreign ball, so pass 1
	// puts it in mustMove[t] and pass 2 gives it an edge to all of mustMove[t] -- including
	// itself. A self-loop is a cycle of length one, so no DFVS can omit it. Hence the
	// intersection is all of notOptimalHome and the subtraction is unconditional.
	//
	// Same value, without reconstructing the set -- which matters, because recovering a
	// minimum set costs many extra exact solves per state and the bound is evaluated on
	// every generated node. test_dfvs.cpp asserts the two formulations agree.
	return MinimumDFVSSize(g)-static_cast<int>(notOptimalHome.size());
}

// The literal form of their formula: compute a minimum DFVS set and subtract the
// notOptimalHome members that appear in it. Kept so the fast path above can be checked
// against the shape their code actually has, rather than only against its result.
template <int numColors, int tubeHeight>
int DFVSLowerBoundViaSet(const BallSortState<numColors, tubeHeight> &s)
{
	const int numSlots = (numColors+1)*tubeHeight;
	if (numSlots > 64)
		return 0;

	std::vector<int> notOptimalHome;
	for (int t = 1; t <= numColors; t++)
	{
		bool tubeOptimal = true;
		for (int k = 0; k < tubeHeight; k++)
		{
			int color = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;
			if (color != t)
				tubeOptimal = false;
			else if (!tubeOptimal)
				notOptimalHome.push_back(t*tubeHeight+k);
		}
	}

	DFVSGraph g = BuildDFVSGraph<numColors, tubeHeight>(s);
	std::vector<int> chosen = MinimumDFVSSet(g);

	uint64_t chosenMask = 0;
	for (int v : chosen)
		chosenMask |= (1ull<<v);

	int bound = static_cast<int>(chosen.size());
	for (int v : notOptimalHome)
		if ((chosenMask>>v)&1ull)
			bound--;
	return bound;
}

/**
 * Their LOWER-BOUND: exactly `TubeRack::lower_bound_for_needed_operations(false)`, the
 * function Algorithm 1 calls on every generated configuration.
 *
 *     LOWER-BOUND(S) = basic_lower_bound(S) + dfvs_lower_bound(S)
 *
 * Because the DFVS term is non-negative, this dominates the simple bound by construction --
 * which is what their §5 reports when it says the DFVS-based bound outperforms the others.
 */
template <int numColors, int tubeHeight>
int PaperLowerBound(const BallSortState<numColors, tubeHeight> &s)
{
	return BasicLowerBound<numColors, tubeHeight>(s)+DFVSLowerBound<numColors, tubeHeight>(s);
}

/**
 * The bound as a HOG2 heuristic, with memoization.
 *
 * The cache matters: their solver calls LOWER-BOUND on every configuration of every BFS
 * level and threw sixteen threads at the cost. We are single-threaded per CLAUDE.md, so
 * repeated states must not pay for the solver twice. Keyed on the environment's perfect
 * hash, so the cache is exact rather than approximate.
 *
 * Only the standard goal is supported, like BallSort::HCost -- an arbitrary goal state
 * would need a different construction entirely.
 */
template <int numColors, int tubeHeight>
class BallSortDFVSHeuristic : public Heuristic<BallSortState<numColors, tubeHeight>> {
public:
	typedef BallSortState<numColors, tubeHeight> State;

	explicit BallSortDFVSHeuristic(const BallSort<numColors, tubeHeight> *environment)
		:env(environment) {}

	double HCost(const State &a, const State &b) const override
	{
		uint64_t key = env->GetStateHash(a);
		auto it = cache.find(key);
		if (it != cache.end())
			return it->second;
		int bound = PaperLowerBound<numColors, tubeHeight>(a);
		cache[key] = bound;
		return bound;
	}

	size_t CacheSize() const { return cache.size(); }
	void ClearCache() const { cache.clear(); }

private:
	const BallSort<numColors, tubeHeight> *env;
	mutable std::unordered_map<uint64_t, int> cache;
};

#endif // BALLSORT_DFVS_BOUND_H
