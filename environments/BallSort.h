//
//  BallSort.h
//
//  The "colored balls in colored tubes" domain, faithful to Althaus et al.
//
//  Tubes are indexed 0..numColors. Tube 0 is the uncolored reserve; tube i (i >= 1)
//  is the goal tube for color i. Every tube holds at most tubeHeight balls.
//
//  A move (i, j) takes the top ball of tube i and puts it on tube j. It is legal iff
//  tube i is non-empty and tube j is not full. There is deliberately NO color matching
//  and NO per-tube color restriction: any top ball may go onto any non-full tube.
//
//  Goal: the reserve is empty and tube i is full of color i.
//
//  Modeled on hog2/environments/TOH.h, but without PDBHeuristic.h -- the heuristic here
//  is hand-written, not a pattern database.
//

#ifndef BallSort_h
#define BallSort_h

#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string>
#include <vector>

#include "SearchEnvironment.h"

struct BallSortMove {
	BallSortMove(uint8_t s, uint8_t d) :source(s), dest(d) {}
	BallSortMove() :source(0), dest(0) {}
	uint8_t source;
	uint8_t dest;
};

// True iff base^exp is representable in a uint64_t. Used to reject template
// parameters whose state encoding would silently wrap around.
constexpr bool BallSortHashFits(uint64_t base, int exp)
{
	uint64_t r = 1;
	for (int i = 0; i < exp; i++)
	{
		if (r > (~0ull) / base)
			return false;
		r *= base;
	}
	return true;
}

constexpr uint64_t BallSortPow(uint64_t base, int exp)
{
	uint64_t r = 1;
	for (int i = 0; i < exp; i++)
		r *= base;
	return r;
}

/**
 * A ball-sort configuration.
 *
 * balls[t][k] is the color at height k of tube t, counting from the bottom, and is
 * only meaningful for k < counts[t]. Colors are 1..numColors; 0 marks an empty slot.
 * Balls are always packed against the bottom of the tube, so a state has exactly one
 * representation -- which is what makes the hash below injective.
 */
template <int numColors, int tubeHeight>
struct BallSortState {
	static const int numTubes = numColors+1;

	BallSortState()
	{
		Reset();
	}

	// The goal configuration: reserve empty, tube i full of color i.
	void Reset()
	{
		for (int t = 0; t < numTubes; t++)
		{
			counts[t] = 0;
			for (int k = 0; k < tubeHeight; k++)
				balls[t][k] = 0;
		}
		for (int t = 1; t < numTubes; t++)
		{
			for (int k = 0; k < tubeHeight; k++)
				balls[t][k] = t;
			counts[t] = tubeHeight;
		}
	}

	/**
	 * Build a start state from a flat list of numColors*tubeHeight colors: the first
	 * tubeHeight entries fill tube 1 bottom-up, the next tubeHeight fill tube 2, and so
	 * on. The reserve is left empty, so every colored tube ends up full -- exactly the
	 * shape of start state the domain calls for.
	 */
	void SetFromColorSequence(const std::vector<int> &colors)
	{
		for (int t = 0; t < numTubes; t++)
		{
			counts[t] = 0;
			for (int k = 0; k < tubeHeight; k++)
				balls[t][k] = 0;
		}
		for (int t = 1; t < numTubes; t++)
		{
			for (int k = 0; k < tubeHeight; k++)
				balls[t][k] = colors[(t-1)*tubeHeight+k];
			counts[t] = tubeHeight;
		}
	}

	int GetBallCountInTube(int whichTube) const { return counts[whichTube]; }
	int GetBallInTube(int whichTube, int whichBall) const { return balls[whichTube][whichBall]; }
	bool IsEmpty(int whichTube) const { return counts[whichTube] == 0; }
	bool IsFull(int whichTube) const { return counts[whichTube] == tubeHeight; }

	// Color of the top ball, or 0 if the tube is empty.
	int GetTopBall(int whichTube) const
	{
		if (counts[whichTube] == 0)
			return 0;
		return balls[whichTube][counts[whichTube]-1];
	}

	uint8_t balls[numColors+1][tubeHeight];
	uint8_t counts[numColors+1];
};

template <int C, int H>
static std::ostream &operator<<(std::ostream &out, const BallSortState<C, H> &s)
{
	for (int t = 0; t < C+1; t++)
	{
		out << "(" << t << ") ";
		for (int k = 0; k < s.GetBallCountInTube(t); k++)
			out << s.GetBallInTube(t, k) << " ";
	}
	return out;
}

template <int C, int H>
static bool operator==(const BallSortState<C, H> &l1, const BallSortState<C, H> &l2)
{
	for (int t = 0; t < C+1; t++)
	{
		if (l1.GetBallCountInTube(t) != l2.GetBallCountInTube(t))
			return false;
		for (int k = 0; k < l1.GetBallCountInTube(t); k++)
		{
			if (l1.GetBallInTube(t, k) != l2.GetBallInTube(t, k))
				return false;
		}
	}
	return true;
}

template <int C, int H>
static bool operator!=(const BallSortState<C, H> &l1, const BallSortState<C, H> &l2)
{
	return !(l1 == l2);
}

static std::ostream &operator<<(std::ostream &out, const BallSortMove &m)
{
	out << "(" << +m.source << ", " << +m.dest << ")";
	return out;
}

static bool operator==(const BallSortMove &m1, const BallSortMove &m2)
{
	return m1.source == m2.source && m1.dest == m2.dest;
}

static bool operator!=(const BallSortMove &m1, const BallSortMove &m2)
{
	return !(m1 == m2);
}

// HOG2's BFS keeps its closed list in a std::unordered_map<state, bool>, so the state
// needs to be hashable. Same mixed-radix packing as BallSort::GetStateHash; it may wrap
// for large templates, which is harmless here since equality still resolves collisions.
namespace std {
	template <int C, int H>
	struct hash<BallSortState<C, H>>
	{
		std::size_t operator()(const BallSortState<C, H> &s) const
		{
			uint64_t h = 0;
			for (int t = C; t >= 0; t--)
			{
				for (int k = H-1; k >= 0; k--)
				{
					uint64_t digit = (k < s.GetBallCountInTube(t)) ? s.GetBallInTube(t, k) : 0;
					h = h*static_cast<uint64_t>(C+1)+digit;
				}
			}
			return static_cast<std::size_t>(h);
		}
	};
}


template <int numColors, int tubeHeight>
class BallSort : public SearchEnvironment<BallSortState<numColors, tubeHeight>, BallSortMove> {
public:
	typedef BallSortState<numColors, tubeHeight> State;

	static const int numTubes = numColors+1;
	// One slot per (tube, height) pair, each holding 0 (empty) or a color 1..numColors.
	static const int numSlots = numTubes*tubeHeight;
	static const uint64_t hashRadix = numColors+1;

	static_assert(BallSortHashFits(hashRadix, numSlots),
				  "BallSort: (numColors+1)^((numColors+1)*tubeHeight) overflows uint64_t; "
				  "use smaller numColors/tubeHeight or a different encoding");

	BallSort() {}
	~BallSort() {}

	void GetSuccessors(const State &nodeID, std::vector<State> &neighbors) const;
	void GetActions(const State &nodeID, std::vector<BallSortMove> &actions) const;
	void ApplyAction(State &s, BallSortMove a) const;
	void UndoAction(State &s, BallSortMove a) const;
	bool InvertAction(BallSortMove &a) const;

	double HCost(const State &node1, const State &node2) const;
	double HCost(const State &node) const;
	double GCost(const State &node1, const State &node2) const { return 1; }
	double GCost(const State &node, const BallSortMove &act) const { return 1; }
	bool GoalTest(const State &node, const State &goal) const;
	bool GoalTest(const State &node) const;

	uint64_t GetStateHash(const State &node) const;
	void GetStateFromHash(uint64_t hash, State &s) const;
	uint64_t GetMaxHash() const { return BallSortPow(hashRadix, numSlots); }
	uint64_t GetNumStates() const { return GetMaxHash(); }
	uint64_t GetActionHash(BallSortMove act) const;

	std::string GetName()
	{
		return "BallSort("+std::to_string(numColors)+","+std::to_string(tubeHeight)+")";
	}

	// The goal configuration, for algorithms that need it explicitly (e.g. bidirectional search).
	State GetGoalState() const { State g; g.Reset(); return g; }

private:
	// caches, mirroring TOH's approach
	mutable std::vector<BallSortMove> acts;
	mutable State tmp;
};


template <int numColors, int tubeHeight>
void BallSort<numColors, tubeHeight>::GetSuccessors(const State &nodeID, std::vector<State> &neighbors) const
{
	neighbors.resize(0);
	GetActions(nodeID, acts);
	for (auto act : acts)
	{
		this->GetNextState(nodeID, act, tmp);
		neighbors.push_back(tmp);
	}
}

/**
 * Every (source, dest) pair with a non-empty source and a non-full destination.
 * No color matching and no per-tube restriction, so the branching factor is high:
 * up to numTubes*(numTubes-1) moves.
 */
template <int numColors, int tubeHeight>
void BallSort<numColors, tubeHeight>::GetActions(const State &s, std::vector<BallSortMove> &actions) const
{
	actions.resize(0);
	for (int i = 0; i < numTubes; i++)
	{
		if (s.IsEmpty(i))
			continue;
		for (int j = 0; j < numTubes; j++)
		{
			if (i == j || s.IsFull(j))
				continue;
			actions.push_back(BallSortMove(i, j));
		}
	}
}

template <int numColors, int tubeHeight>
void BallSort<numColors, tubeHeight>::ApplyAction(State &s, BallSortMove m) const
{
	s.balls[m.dest][s.counts[m.dest]] = s.balls[m.source][s.counts[m.source]-1];
	s.counts[m.dest]++;
	s.counts[m.source]--;
	s.balls[m.source][s.counts[m.source]] = 0;
}

template <int numColors, int tubeHeight>
void BallSort<numColors, tubeHeight>::UndoAction(State &s, BallSortMove m) const
{
	s.balls[m.source][s.counts[m.source]] = s.balls[m.dest][s.counts[m.dest]-1];
	s.counts[m.source]++;
	s.counts[m.dest]--;
	s.balls[m.dest][s.counts[m.dest]] = 0;
}

template <int numColors, int tubeHeight>
bool BallSort<numColors, tubeHeight>::InvertAction(BallSortMove &a) const
{
	uint8_t t = a.source;
	a.source = a.dest;
	a.dest = t;
	return true;
}

/**
 * Number of balls not currently sitting in their own goal tube.
 *
 * Admissible: a move relocates exactly one ball, so it can reduce this count by at
 * most 1; hence count <= h*. Consistent: any single move changes the count by at most
 * 1 while costing 1, so |h(n) - h(n')| <= 1 = c(n, n').
 *
 * This is the fallback heuristic named in the project brief, not one lifted from the
 * paper -- the paper's bound (if it has a usable one) should replace or refine it.
 * Balls in the reserve are misplaced by definition, since the goal requires it empty.
 */
template <int numColors, int tubeHeight>
double BallSort<numColors, tubeHeight>::HCost(const State &node) const
{
	int misplaced = 0;
	for (int t = 0; t < numTubes; t++)
	{
		for (int k = 0; k < node.GetBallCountInTube(t); k++)
		{
			if (node.GetBallInTube(t, k) != t)
				misplaced++;
		}
	}
	return misplaced;
}

template <int numColors, int tubeHeight>
double BallSort<numColors, tubeHeight>::HCost(const State &node1, const State &node2) const
{
	// Only the standard goal is supported; arbitrary goal states would need a different bound.
	return HCost(node1);
}

template <int numColors, int tubeHeight>
bool BallSort<numColors, tubeHeight>::GoalTest(const State &node) const
{
	if (!node.IsEmpty(0))
		return false;
	for (int t = 1; t < numTubes; t++)
	{
		if (!node.IsFull(t))
			return false;
		for (int k = 0; k < tubeHeight; k++)
		{
			if (node.GetBallInTube(t, k) != t)
				return false;
		}
	}
	return true;
}

template <int numColors, int tubeHeight>
bool BallSort<numColors, tubeHeight>::GoalTest(const State &node, const State &goal) const
{
	return GoalTest(node);
}

/**
 * Perfect, injective bit-packing in the style of TOH's hash.
 *
 * Each of the numSlots (tube, height) slots holds a value in 0..numColors, so the state
 * is a numSlots-digit number in base (numColors+1). Because balls are always packed
 * against the bottom of a tube, each configuration has exactly one digit string, and
 * distinct configurations give distinct hashes. GetMaxHash() is a strict upper bound;
 * the range is not dense, since digit strings with a gap in a tube are unreachable.
 */
template <int numColors, int tubeHeight>
uint64_t BallSort<numColors, tubeHeight>::GetStateHash(const State &node) const
{
	uint64_t hash = 0;
	for (int t = numTubes-1; t >= 0; t--)
	{
		for (int k = tubeHeight-1; k >= 0; k--)
		{
			uint64_t digit = (k < node.GetBallCountInTube(t)) ? node.GetBallInTube(t, k) : 0;
			hash = hash*hashRadix+digit;
		}
	}
	return hash;
}

template <int numColors, int tubeHeight>
void BallSort<numColors, tubeHeight>::GetStateFromHash(uint64_t hash, State &s) const
{
	for (int t = 0; t < numTubes; t++)
	{
		s.counts[t] = 0;
		for (int k = 0; k < tubeHeight; k++)
			s.balls[t][k] = 0;
	}
	for (int t = 0; t < numTubes; t++)
	{
		for (int k = 0; k < tubeHeight; k++)
		{
			uint64_t digit = hash%hashRadix;
			hash /= hashRadix;
			s.balls[t][k] = static_cast<uint8_t>(digit);
			if (digit != 0)
				s.counts[t] = k+1;
		}
	}
}

template <int numColors, int tubeHeight>
uint64_t BallSort<numColors, tubeHeight>::GetActionHash(BallSortMove act) const
{
	return (static_cast<uint64_t>(act.source)<<8)|act.dest;
}

#endif /* BallSort_h */
