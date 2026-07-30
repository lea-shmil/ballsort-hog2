// dfvs_exact.h -- exact minimum directed feedback vertex set, for small dense-ish graphs.
//
// Why this exists: the lower bound Althaus et al. use in their solver is the size of a
// *minimum DFVS* of a graph built from the tube-rack configuration (see dfvs_bound.h).
// Their implementation calls the PACE 2022 exact-track solver "rubengoetz", vendored into
// their repository. We do not vendor it -- their repository carries no license (see the
// README's provenance section) -- so this is our own exact solver.
//
// Fidelity note: a minimum is a minimum. Any two *exact* DFVS solvers return sets of the
// same cardinality on the same graph, so the bound values this produces are identical to
// theirs by construction, even though none of the code is. Only the runtime differs.
//
// Scope: the graphs here have one vertex per tube slot, so n = numTubes*tubeHeight <= 42
// across the whole paper grid. That is small enough for exact branch and bound and lets
// the whole graph live in uint64_t bitmasks. n > 64 is rejected at the call site.
//
// Structure of the solver, standard for this problem:
//   1. reduction   -- self-loops are forced into every solution; a vertex with no live
//                     in-edge or no live out-edge lies on no cycle and can be dropped.
//   2. decomposition -- a DFVS decomposes over strongly connected components, so each SCC
//                     is solved independently and the sizes add.
//   3. branch and bound -- every DFVS must hit every cycle, so branching over the vertices
//                     of one (shortest) cycle is exhaustive. Bounded below by a packing of
//                     vertex-disjoint cycles, each of which needs a distinct vertex.

#ifndef BALLSORT_DFVS_EXACT_H
#define BALLSORT_DFVS_EXACT_H

#include <cstdint>
#include <vector>

// A directed graph on n <= 64 vertices. out[v] is the bitmask of v's successors; a set bit
// v in out[v] is a self-loop, which matters here -- their construction creates them on
// purpose (see dfvs_bound.h).
struct DFVSGraph {
	int n = 0;
	std::vector<uint64_t> out;

	explicit DFVSGraph(int numVertices) :n(numVertices), out(numVertices, 0) {}

	void AddEdge(int from, int to) { out[from] |= (1ull<<to); }
	bool HasEdge(int from, int to) const { return (out[from]>>to)&1ull; }
};

namespace dfvs_detail {

inline int PopCount(uint64_t x) { return __builtin_popcountll(x); }
inline int LowestBit(uint64_t x) { return __builtin_ctzll(x); }

// In-neighbour masks restricted to `alive`.
inline void BuildInMasks(const DFVSGraph &g, uint64_t alive, std::vector<uint64_t> &in)
{
	in.assign(g.n, 0);
	uint64_t rest = alive;
	while (rest)
	{
		int v = LowestBit(rest);
		rest &= rest-1;
		uint64_t succ = g.out[v]&alive;
		while (succ)
		{
			int u = LowestBit(succ);
			succ &= succ-1;
			in[u] |= (1ull<<v);
		}
	}
}

/**
 * Apply the two safe reduction rules until nothing changes.
 *
 * - A vertex with a self-loop is on a cycle of length 1, so every DFVS contains it. Count
 *   it and remove it.
 * - A vertex with no live in-edge, or no live out-edge, is on no cycle at all. Remove it
 *   without cost. (Self-loops are handled first, so a self-loop vertex is never dropped
 *   here by mistake.)
 *
 * Returns the number of vertices forced into the solution; `alive` is narrowed in place.
 */
inline int Reduce(const DFVSGraph &g, uint64_t &alive)
{
	int forced = 0;
	bool changed = true;
	while (changed)
	{
		changed = false;

		uint64_t rest = alive;
		while (rest)
		{
			int v = LowestBit(rest);
			rest &= rest-1;
			if ((g.out[v]>>v)&1ull) // self-loop
			{
				alive &= ~(1ull<<v);
				forced++;
				changed = true;
			}
		}
		if (changed)
			continue;

		std::vector<uint64_t> in;
		BuildInMasks(g, alive, in);
		rest = alive;
		while (rest)
		{
			int v = LowestBit(rest);
			rest &= rest-1;
			if ((g.out[v]&alive) == 0 || in[v] == 0)
			{
				alive &= ~(1ull<<v);
				changed = true;
			}
		}
	}
	return forced;
}

// Strongly connected components of the subgraph induced on `alive`, as bitmasks. Iterative
// Tarjan -- the recursion depth would otherwise be the vertex count, which is fine at 42
// but the iterative form costs nothing here.
inline std::vector<uint64_t> StronglyConnectedComponents(const DFVSGraph &g, uint64_t alive)
{
	std::vector<uint64_t> components;
	std::vector<int> index(g.n, -1), low(g.n, 0);
	std::vector<bool> onStack(g.n, false);
	std::vector<int> stack;
	int nextIndex = 0;

	// (vertex, remaining successors to visit)
	std::vector<std::pair<int, uint64_t>> call;

	uint64_t rest = alive;
	while (rest)
	{
		int root = LowestBit(rest);
		rest &= rest-1;
		if (index[root] != -1)
			continue;

		call.push_back({root, g.out[root]&alive});
		index[root] = low[root] = nextIndex++;
		stack.push_back(root);
		onStack[root] = true;

		while (!call.empty())
		{
			int v = call.back().first;
			uint64_t &todo = call.back().second;

			if (todo)
			{
				int w = LowestBit(todo);
				todo &= todo-1;
				if (index[w] == -1)
				{
					index[w] = low[w] = nextIndex++;
					stack.push_back(w);
					onStack[w] = true;
					call.push_back({w, g.out[w]&alive});
				}
				else if (onStack[w])
				{
					if (index[w] < low[v])
						low[v] = index[w];
				}
				continue;
			}

			call.pop_back();
			if (!call.empty())
			{
				int parent = call.back().first;
				if (low[v] < low[parent])
					low[parent] = low[v];
			}
			if (low[v] == index[v])
			{
				uint64_t component = 0;
				while (true)
				{
					int w = stack.back();
					stack.pop_back();
					onStack[w] = false;
					component |= (1ull<<w);
					if (w == v)
						break;
				}
				// Single vertices with no self-loop are not cycles; self-loops are already
				// gone by the time this runs, so drop them.
				if (PopCount(component) > 1)
					components.push_back(component);
			}
		}
	}
	return components;
}

/**
 * Shortest directed cycle within `alive`, as a list of vertices. BFS from every vertex,
 * keeping the best. A shortest cycle keeps the branching factor small, which is the whole
 * reason to spend the search here rather than branch on an arbitrary cycle.
 *
 * Returns an empty vector if the subgraph is acyclic (which the callers rule out).
 */
inline std::vector<int> ShortestCycle(const DFVSGraph &g, uint64_t alive)
{
	std::vector<int> best;
	std::vector<int> parent(g.n), dist(g.n);

	uint64_t rest = alive;
	while (rest)
	{
		int start = LowestBit(rest);
		rest &= rest-1;

		for (int i = 0; i < g.n; i++) { parent[i] = -1; dist[i] = -1; }
		std::vector<int> queue;
		queue.push_back(start);
		dist[start] = 0;

		for (size_t qi = 0; qi < queue.size(); qi++)
		{
			int v = queue[qi];
			if (!best.empty() && dist[v]+1 >= static_cast<int>(best.size()))
				break; // cannot beat the incumbent
			uint64_t succ = g.out[v]&alive;
			while (succ)
			{
				int w = LowestBit(succ);
				succ &= succ-1;
				if (w == start)
				{
					// Found a cycle back to the start: walk the parent chain.
					std::vector<int> cycle;
					for (int x = v; x != -1; x = parent[x])
						cycle.push_back(x);
					if (best.empty() || cycle.size() < best.size())
						best = cycle;
					break;
				}
				if (dist[w] == -1)
				{
					dist[w] = dist[v]+1;
					parent[w] = v;
					queue.push_back(w);
				}
			}
			if (best.size() == 2)
				return best; // a 2-cycle is as short as it gets once self-loops are gone
		}
	}
	return best;
}

// Greedy packing of vertex-disjoint cycles. Each cycle in the packing needs its own DFVS
// vertex, so the packing size is a lower bound on the remaining solution -- the pruning
// bound for the branch and bound below.
inline int DisjointCycleLowerBound(const DFVSGraph &g, uint64_t alive)
{
	int bound = 0;
	uint64_t remaining = alive;
	while (true)
	{
		uint64_t sub = remaining;
		int forcedIgnored = 0;
		(void)forcedIgnored;
		// Trim vertices that cannot be on a cycle inside `sub`, or ShortestCycle wastes time.
		bool changed = true;
		while (changed)
		{
			changed = false;
			std::vector<uint64_t> in;
			BuildInMasks(g, sub, in);
			uint64_t rest = sub;
			while (rest)
			{
				int v = LowestBit(rest);
				rest &= rest-1;
				if ((g.out[v]&sub) == 0 || in[v] == 0)
				{
					sub &= ~(1ull<<v);
					changed = true;
				}
			}
		}
		if (sub == 0)
			break;
		std::vector<int> cycle = ShortestCycle(g, sub);
		if (cycle.empty())
			break;
		bound++;
		for (int v : cycle)
			remaining &= ~(1ull<<v);
	}
	return bound;
}

// Branch and bound on one strongly connected subgraph. `best` is the incumbent for the
// whole subproblem; returns the exact minimum, or something >= best if it proved it cannot
// beat the incumbent (safe, because the caller only keeps improvements).
inline int SolveComponent(const DFVSGraph &g, uint64_t alive, int best)
{
	int forced = Reduce(g, alive);
	if (forced >= best)
		return forced;         // already no better than the incumbent
	if (alive == 0)
		return forced;

	std::vector<uint64_t> components = StronglyConnectedComponents(g, alive);
	if (components.empty())
		return forced;

	if (components.size() > 1)
	{
		int total = forced;
		for (uint64_t component : components)
		{
			if (total >= best)
				return total;
			total += SolveComponent(g, component, best-total);
		}
		return total;
	}

	uint64_t component = components[0];
	if (forced+DisjointCycleLowerBound(g, component) >= best)
		return best;           // pruned

	std::vector<int> cycle = ShortestCycle(g, component);
	if (cycle.empty())
		return forced;

	int localBest = best;
	for (int v : cycle)
	{
		int value = forced+1+SolveComponent(g, component&~(1ull<<v), localBest-forced-1);
		if (value < localBest)
			localBest = value;
	}
	return localBest;
}

} // namespace dfvs_detail

/**
 * Size of a minimum directed feedback vertex set of `g`: the fewest vertices whose removal
 * makes the graph acyclic. Exact.
 *
 * Requires g.n <= 64 (bitmask representation).
 */
inline int MinimumDFVSSize(const DFVSGraph &g)
{
	if (g.n <= 0)
		return 0;
	uint64_t alive = (g.n == 64) ? ~0ull : ((1ull<<g.n)-1);
	// An upper bound of n+1 is a valid "no incumbent yet" sentinel: no DFVS exceeds n.
	return dfvs_detail::SolveComponent(g, alive, g.n+1);
}

/**
 * Which vertices a minimum DFVS contains, not just how many.
 *
 * Needed because their bound is not simply |DFVS| -- it subtracts the DFVS members that
 * are home-tube balls out of final position (see dfvs_bound.h), so the identity of the
 * chosen vertices matters. Any minimum set is as good as any other for their formula's
 * *count*, because every vertex it subtracts carries a self-loop and is therefore in every
 * minimum DFVS; this returns one such set.
 *
 * Built by taking forced (self-loop) vertices plus a greedy completion driven by the exact
 * size, so the result is a genuine minimum set rather than a heuristic one.
 */
inline std::vector<int> MinimumDFVSSet(const DFVSGraph &g)
{
	std::vector<int> chosen;
	if (g.n <= 0)
		return chosen;

	uint64_t alive = (g.n == 64) ? ~0ull : ((1ull<<g.n)-1);

	// Self-loop vertices are in every DFVS; take them first and record them.
	bool changed = true;
	while (changed)
	{
		changed = false;
		uint64_t rest = alive;
		while (rest)
		{
			int v = dfvs_detail::LowestBit(rest);
			rest &= rest-1;
			if ((g.out[v]>>v)&1ull)
			{
				chosen.push_back(v);
				alive &= ~(1ull<<v);
				changed = true;
			}
		}
	}

	// Complete to a minimum set: repeatedly pick a vertex that keeps the remaining exact
	// optimum one smaller, which is exactly the definition of being in some minimum set.
	while (true)
	{
		uint64_t work = alive;
		dfvs_detail::Reduce(g, work);
		std::vector<uint64_t> components = dfvs_detail::StronglyConnectedComponents(g, work);
		if (components.empty())
			break;

		int target = dfvs_detail::SolveComponent(g, alive, g.n+1);
		if (target == 0)
			break;

		std::vector<int> cycle = dfvs_detail::ShortestCycle(g, components[0]);
		bool picked = false;
		for (int v : cycle)
		{
			uint64_t without = alive&~(1ull<<v);
			if (dfvs_detail::SolveComponent(g, without, g.n+1) == target-1)
			{
				chosen.push_back(v);
				alive = without;
				picked = true;
				break;
			}
		}
		if (!picked)
			break; // cannot happen for an exact `target`, but never loop forever
	}
	return chosen;
}

#endif // BALLSORT_DFVS_EXACT_H
