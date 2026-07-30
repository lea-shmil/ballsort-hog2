// instance_gen.h -- generating and reading BallSort start instances.
//
// ATTRIBUTION. The benchmark design in this file is not ours. It comes from
//
//   Althaus, E.; Blumenstock, M.; Rassau, N.; Schuhknecht, F. M.; and Zimdars, A. Q. 2025.
//   Sorting colored balls in colored tubes. In Proceedings of the 18th International
//   Symposium on Combinatorial Search (SoCS 2025), 11-19. Glasgow, United Kingdom:
//   AAAI Press.   https://ojs.aaai.org/index.php/SOCS/article/view/35971
//   Code and datasets: https://gitlab.rlp.net/rassau/sorting-colored-balls
//
// Specifically theirs, reimplemented here rather than copied: the instance-generation
// procedure, the ten-per-cell experimental protocol, the .in file format, the {h}x{T}
// cell and file naming, the 53-cell parameter grid, the simple lower bound, and the
// reachable-configuration count. Each is marked at its definition below. Ours: the
// seeding scheme, the verification, the difficulty tiering, and the reader.
//
// No source or data file of theirs is vendored here -- their repository is public but
// unlicensed, so everything below was written from the paper's description and from
// inspecting their published artifacts. See the "Provenance and attribution" section of
// the README.
//
// Deliberately independent of BallSort.h / HOG2: a start instance is just a sequence of
// c*h colors (h copies each of 1..c) in some order, read bottom-to-top into tubes
// 1..c via BallSortState::SetFromColorSequence. Per CLAUDE.md, every full-tube
// arrangement with exactly h balls per color is solvable, so no solvability check is
// needed here -- any permutation of the multiset is a valid instance.
//
// What the paper actually does (Definition 2, "RSCBT", and Section 7):
//
//  - Height profile is uniform: every tube, reserve included, has height h. The start
//    configuration has |S_0| = 0 and |S_i| = h for 1 <= i <= c. That is exactly our
//    start state, so the domain needs no adjustment to match.
//  - Instances are "randomly generated initial tube-rack configurations": a uniform
//    shuffle of the multiset {h copies of each color 1..c}, sliced into the c colored
//    tubes in order. Their generator (.vscode/generate_examples.py in the paper's
//    repository) is exactly a python random.shuffle of that multiset -- no solvability
//    check, no difficulty filter, no rejection sampling.
//  - Ten instances per parameter cell, and the reported figure is the *median* over
//    those ten runs. Section 7: "For each parameter configuration (h, c+1) and metric
//    of interest, we perform ten runs on ten different randomly generated initial
//    tube-rack configurations and report the median."
//  - Cells are indexed by (h, numTubes) with numTubes = c+1, i.e. the reserve is counted.
//    Their published benchmark set is named accordingly: resources/paper_inputs/{h}x{T}/
//    random_generated_{h}x{T}_{i}.in for i in 0..9. PaperGridCells() below is that grid.
//
// Two deliberate differences from their generator, both in the direction of being
// stricter, are marked DEVIATION at the point where they happen: our shuffle is seeded
// (theirs is not, so their instances are only reproducible as files, not from a seed),
// and by default we reject a drawn arrangement that is already the goal configuration.

#ifndef BALLSORT_INSTANCE_GEN_H
#define BALLSORT_INSTANCE_GEN_H

#include <algorithm>
#include <cstdint>
#include <istream>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Drawing an instance
// ---------------------------------------------------------------------------

// h copies each of colors 1..c, uniformly shuffled. This is the whole of the paper's
// instance generation -- theirs, from paper §7 and confirmed line by line against their
// .vscode/generate_examples.py, which is a bare python random.shuffle of this multiset.
inline std::vector<int> RandomColorSequence(int numColors, int tubeHeight, std::mt19937 &rng)
{
	std::vector<int> colors;
	colors.reserve(numColors*tubeHeight);
	for (int c = 1; c <= numColors; c++)
		for (int k = 0; k < tubeHeight; k++)
			colors.push_back(c);
	std::shuffle(colors.begin(), colors.end(), rng);
	return colors;
}

// Confirms a sequence is exactly h copies each of 1..c -- catches a broken RNG or a
// generator bug before it produces a file of instances an experiment run is trusted to
// consume. Fisher-Yates shuffling a correctly-built multiset can't fail this by
// construction, but the check is cheap and this is the one tool where a silent bug
// would be expensive to discover after the fact.
inline bool VerifyColorSequence(const std::vector<int> &colors, int numColors, int tubeHeight)
{
	if (static_cast<int>(colors.size()) != numColors*tubeHeight)
		return false;
	std::vector<int> counts(numColors+1, 0);
	for (int c : colors)
	{
		if (c < 1 || c > numColors)
			return false;
		counts[c]++;
	}
	for (int c = 1; c <= numColors; c++)
		if (counts[c] != tubeHeight)
			return false;
	return true;
}

// DEVIATION (seeding). The paper's generator calls python's random.shuffle unseeded, so
// its instances are reproducible only as the published files. We derive each instance's
// seed from (masterSeed, tubeHeight, numTubes, index) instead, which buys two things a
// single sequential stream does not:
//
//  - cells are independent, so generating cell 4x8 does not depend on having generated
//    3x5 first, and a job array can regenerate one cell on one node;
//  - the set is *extensible*: instance i is a function of i alone, so raising the count
//    per cell from 10 to 30 leaves instances 0..9 byte-identical. Instances 0..9 stay
//    the subset directly comparable to the paper's ten.
//
// splitmix64's finalizer, mixed over the four fields -- a plain sum or concatenation
// would give neighbouring cells adjacent, and therefore correlated, mt19937 seeds.
inline uint64_t InstanceSeed(uint64_t masterSeed, int tubeHeight, int numTubes, int index)
{
	auto mix = [](uint64_t z) {
		z += 0x9e3779b97f4a7c15ull;
		z = (z^(z>>30))*0xbf58476d1ce4e5b9ull;
		z = (z^(z>>27))*0x94d049bb133111ebull;
		return z^(z>>31);
	};
	uint64_t s = mix(masterSeed);
	s = mix(s^(static_cast<uint64_t>(tubeHeight)*0x100000001b3ull));
	s = mix(s^(static_cast<uint64_t>(numTubes)*0xff51afd7ed558ccdull));
	s = mix(s^static_cast<uint64_t>(index));
	return s;
}

// ---------------------------------------------------------------------------
// Difficulty metadata
// ---------------------------------------------------------------------------

/**
 * The paper's "simple lower bound" on the number of moves (Section 6, illustrated in
 * their Figure 4), evaluated on a start configuration.
 *
 * Definition 3 of the paper: a ball of color i is in *final position* if it is in tube i
 * and every ball below it in tube i is also of color i. From that:
 *
 *  - a ball in final position never has to move:                  0
 *  - a ball of color i in tube i but not in final position has to
 *    leave tube i and come back, because something wrong is
 *    underneath it:                                               2
 *  - a ball anywhere else has to move at least once:               1
 *
 * A move relocates exactly one ball, so summing these per-ball minima is a valid lower
 * bound on the total number of moves. This is the paper's weakest bound -- they report
 * their DFVS-based bound dominating it -- but it needs no solver, which is what makes it
 * usable here as a per-instance difficulty tag.
 *
 * `colors` is the flat start sequence: tube t (1-based) holds colors[(t-1)*h .. t*h-1]
 * bottom-to-top, and the reserve is empty.
 */
inline int SimpleLowerBound(const std::vector<int> &colors, int numColors, int tubeHeight)
{
	int bound = 0;
	for (int t = 1; t <= numColors; t++)
	{
		bool stillFinal = true; // every ball so far in this tube, from the bottom, was color t
		for (int k = 0; k < tubeHeight; k++)
		{
			int color = colors[(t-1)*tubeHeight+k];
			if (color != t)
			{
				stillFinal = false;
				bound += 1;         // wrong tube: at least one move
			}
			else if (!stillFinal)
			{
				bound += 2;         // right tube, wrong position: out and back
			}
			// else: in final position, never has to move
		}
	}
	return bound;
}

// Balls already in final position -- the complement view of the bound above, reported in
// the manifest because "how much of the instance is pre-solved" is the direct reading of
// whether a cell's instances are degenerate.
inline int BallsInFinalPosition(const std::vector<int> &colors, int numColors, int tubeHeight)
{
	int count = 0;
	for (int t = 1; t <= numColors; t++)
	{
		for (int k = 0; k < tubeHeight; k++)
		{
			if (colors[(t-1)*tubeHeight+k] != t)
				break;
			count++;
		}
	}
	return count;
}

/**
 * Exact number of reachable tube-rack configurations, the paper's
 *
 *     N = (h+c)! * (hc)! / ( c! * (h!)^(c+1) )
 *
 * from Section 7. The two factors are worth separating, because that is where the
 * formula comes from: C(h+c, c) ways to distribute the h empty slots over the c+1 tubes,
 * times (hc)!/(h!)^c distinct orderings of the ball multiset along the resulting slots.
 *
 * Returned as long double: N exceeds 2^64 well inside the paper's grid (their 2x14 cell
 * is about 5e24), and this value is only ever printed or compared against a threshold.
 */
inline long double ReachableStateCount(int numColors, int tubeHeight)
{
	const int c = numColors, h = tubeHeight;
	long double n = 1.0L;
	// C(h+c, c)
	for (int i = 1; i <= c; i++)
		n = n*static_cast<long double>(h+i)/static_cast<long double>(i);
	// (hc)! / (h!)^c, built as a running product so the intermediate never blows up
	long double perm = 1.0L;
	for (int i = 1; i <= h*c; i++)
		perm *= static_cast<long double>(i);
	for (int j = 0; j < c; j++)
		for (int i = 1; i <= h; i++)
			perm /= static_cast<long double>(i);
	return n*perm;
}

/**
 * Whether BallSort<c,h>'s perfect hash fits a uint64_t: the state is a (c+1)*h digit
 * number in base (c+1), so it needs (c+1)*h*log2(c+1) bits. BallSort.h static_asserts
 * this, so a cell that fails here cannot be compiled at all, let alone searched.
 *
 * Kept here rather than read off BallSort.h because this header is HOG2-free on purpose;
 * it duplicates the *predicate*, not the encoding, and the two agree by construction
 * (both are "(c+1)^((c+1)*h) representable").
 */
inline bool StateHashFitsUint64(int numColors, int tubeHeight)
{
	const uint64_t base = static_cast<uint64_t>(numColors)+1;
	const int slots = (numColors+1)*tubeHeight;
	uint64_t r = 1;
	for (int i = 0; i < slots; i++)
	{
		if (r > (~0ull)/base)
			return false;
		r *= base;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The parameter grid
// ---------------------------------------------------------------------------

// A parameter cell, named the paper's way: tube height and *total* tubes, reserve
// included. numColors is numTubes-1.
struct GridCell {
	int tubeHeight;
	int numTubes;
	int NumColors() const { return numTubes-1; }
	// The paper's cell name and file stem, e.g. "4x8" for h=4 over 8 tubes.
	std::string Name() const
	{
		return std::to_string(tubeHeight)+"x"+std::to_string(numTubes);
	}
};

/**
 * The paper's published benchmark grid: the 53 cells under resources/paper_inputs/ in
 * their repository, transcribed. h runs from 2, numTubes from 3 (two colors -- the
 * smallest non-degenerate case), and each row stops where they stopped generating.
 *
 * Their solver cleared h = 14 at numTubes = 3 and numTubes = 11 at h = 2; cells past
 * that are in the grid but came out as empty boxes in their Figure 6. We keep the whole
 * grid so our sweep covers theirs, and tag reach separately (see CellTier).
 */
inline std::vector<GridCell> PaperGridCells()
{
	// maxTubes[h] -- the largest numTubes the paper generated at that height.
	static const int heights[]  = { 2,  3,  4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
	static const int maxTubes[] = {14, 12,  9, 8, 6, 5, 5, 4,  4,  3,  3,  3,  3};
	std::vector<GridCell> cells;
	for (size_t i = 0; i < sizeof(heights)/sizeof(heights[0]); i++)
		for (int t = 3; t <= maxTubes[i]; t++)
			cells.push_back(GridCell{heights[i], t});
	return cells;
}

/**
 * Difficulty tier of a cell, for this study rather than for the paper's solver.
 *
 * The paper's solver is a 16-thread, disk-backed BFS with a DFVS lower bound; ours is
 * the textbook best-first family, single-threaded per CLAUDE.md, and the *uninformed*
 * members set the ceiling. The tiers are cut on the reachable state count N because
 * that is what BFS, bidirectional BFS, frontier BFS and Dijkstra all pay in full:
 *
 *   Trivial: N < 1e3     -- solved instantly by everything; keeps no algorithm honest.
 *   A:  1e3 <= N < 1e5   -- IDDFS/DFID still finishes. It has no closed list, so it
 *                           re-expands: on BallSort(3,3) (N = 3.4e4) it already cost
 *                           161M expansions and 52s for one instance, which is what
 *                           puts the iterative-deepening ceiling this low.
 *   B:  1e5 <= N < 1e8   -- the memory-bounded optimal algorithms (BFS, biBFS,
 *                           frontier BFS, Dijkstra, A*) all still finish; IDDFS won't.
 *   C:  1e8 <= N < 1e11  -- exhaustive search runs out of memory; A*, IDA*, weighted A*
 *                           and greedy best-first are the only candidates. This is
 *                           where a heuristic has to earn its place.
 *   D:  N >= 1e11        -- beyond reach with the current heuristic. Generated so the
 *                           set covers the paper's grid, not expected to be solved.
 *
 * These are a-priori cuts from N, not measurements; the sweep is what turns them into
 * measured reach, and the numbers above are the one measurement we already have.
 */
enum CellTier { kTierTrivial = 0, kTierA, kTierB, kTierC, kTierD };

inline CellTier CellTierOf(const GridCell &cell)
{
	const long double n = ReachableStateCount(cell.NumColors(), cell.tubeHeight);
	if (n < 1e3L)  return kTierTrivial;
	if (n < 1e5L)  return kTierA;
	if (n < 1e8L)  return kTierB;
	if (n < 1e11L) return kTierC;
	return kTierD;
}

inline const char *CellTierName(CellTier tier)
{
	switch (tier)
	{
		case kTierTrivial: return "trivial";
		case kTierA:       return "A";
		case kTierB:       return "B";
		case kTierC:       return "C";
		case kTierD:       return "D";
	}
	return "?";
}

// The set actually intended for the algorithm comparison: paper cells that our uint64
// perfect hash can encode at all, minus the trivial end and minus tier D. That is the
// "neither too simple nor too complicated" band -- tier A instances are there to give
// IDDFS/DFID something it can finish, and tier C to give the heuristic-guided
// algorithms something the uninformed ones cannot.
inline std::vector<GridCell> CoreGridCells()
{
	std::vector<GridCell> cells;
	for (const GridCell &cell : PaperGridCells())
	{
		if (!StateHashFitsUint64(cell.NumColors(), cell.tubeHeight))
			continue;
		CellTier tier = CellTierOf(cell);
		if (tier == kTierTrivial || tier == kTierD)
			continue;
		cells.push_back(cell);
	}
	return cells;
}

// Every paper cell BallSort<c,h> can be instantiated for, tiers included. Useful for
// asking "what does the encoding, rather than the search, rule out".
inline std::vector<GridCell> RunnableGridCells()
{
	std::vector<GridCell> cells;
	for (const GridCell &cell : PaperGridCells())
		if (StateHashFitsUint64(cell.NumColors(), cell.tubeHeight))
			cells.push_back(cell);
	return cells;
}

// ---------------------------------------------------------------------------
// The paper's file format
// ---------------------------------------------------------------------------
//
// One line per tube: the tube's own id, then h ball colors. Colored tubes 1..c come
// first, then the reserve as tube 0 with h zeros. From their random_generated_4x8_0.in:
//
//     1 6 6 7 2
//     2 4 2 3 5
//     ...
//     7 1 3 5 6
//     0 0 0 0 0
//
// We read and write the h colors bottom-to-top, matching BallSortState's convention.
// Their generator writes a shuffled slice straight out, so for uniformly random
// instances the two readings are the same distribution and the convention only matters
// if their exact published files are ingested -- in which case an instance is read as
// the vertical mirror of theirs, which is still a valid, equally random instance of the
// same cell.

inline void WritePaperInstance(std::ostream &out, const std::vector<int> &colors,
							   int numColors, int tubeHeight)
{
	for (int t = 1; t <= numColors; t++)
	{
		out << t;
		for (int k = 0; k < tubeHeight; k++)
			out << " " << colors[(t-1)*tubeHeight+k];
		out << "\n";
	}
	out << 0;
	for (int k = 0; k < tubeHeight; k++)
		out << " " << 0;
	out << "\n";
}

/**
 * Read one instance in the format above. Returns the flat color sequence, or an empty
 * vector on any malformed input, with `error` set. Strict on purpose: an experiment
 * driver that silently searches a misparsed instance produces numbers that look fine.
 */
inline std::vector<int> ReadPaperInstance(std::istream &in, int &numColorsOut,
										  int &tubeHeightOut, std::string &error)
{
	std::vector<std::vector<int>> rows;
	std::string line;
	while (std::getline(in, line))
	{
		if (line.find_first_not_of(" \t\r\n") == std::string::npos)
			continue;
		std::istringstream ls(line);
		std::vector<int> row;
		int v;
		while (ls >> v)
			row.push_back(v);
		if (!ls.eof())
		{
			error = "non-integer token in tube line";
			return {};
		}
		rows.push_back(row);
	}

	if (rows.size() < 2)
	{
		error = "need at least one colored tube and the reserve";
		return {};
	}
	const int numColors = static_cast<int>(rows.size())-1;
	const int tubeHeight = static_cast<int>(rows[0].size())-1;
	if (tubeHeight < 1)
	{
		error = "tube height must be at least 1";
		return {};
	}

	std::vector<int> colors;
	colors.reserve(numColors*tubeHeight);
	for (int t = 1; t <= numColors; t++)
	{
		const std::vector<int> &row = rows[t-1];
		if (static_cast<int>(row.size()) != tubeHeight+1)
		{
			error = "tube "+std::to_string(t)+" has the wrong number of entries";
			return {};
		}
		if (row[0] != t)
		{
			error = "tube lines must be labelled 1.."+std::to_string(numColors)+" in order";
			return {};
		}
		for (int k = 0; k < tubeHeight; k++)
			colors.push_back(row[k+1]);
	}

	const std::vector<int> &reserve = rows.back();
	if (static_cast<int>(reserve.size()) != tubeHeight+1 || reserve[0] != 0)
	{
		error = "last line must be the reserve tube, labelled 0";
		return {};
	}
	for (int k = 0; k < tubeHeight; k++)
	{
		if (reserve[k+1] != 0)
		{
			error = "reserve tube must start empty (all zeros)";
			return {};
		}
	}

	if (!VerifyColorSequence(colors, numColors, tubeHeight))
	{
		error = "not exactly "+std::to_string(tubeHeight)+" balls of each of "
				+std::to_string(numColors)+" colors";
		return {};
	}

	numColorsOut = numColors;
	tubeHeightOut = tubeHeight;
	error.clear();
	return colors;
}

#endif // BALLSORT_INSTANCE_GEN_H
