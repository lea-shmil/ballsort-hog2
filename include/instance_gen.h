// instance_gen.h -- generating random BallSort start states.
//
// Deliberately independent of BallSort.h / HOG2: a start instance is just a sequence of
// C*H colors (H copies each of 1..C) in some order, read bottom-to-top into tubes
// 1..C via BallSortState::SetFromColorSequence. Per CLAUDE.md, every full-tube
// arrangement with exactly H balls per color is solvable, so no solvability check is
// needed here -- any permutation of the multiset is a valid instance.

#ifndef BALLSORT_INSTANCE_GEN_H
#define BALLSORT_INSTANCE_GEN_H

#include <algorithm>
#include <random>
#include <vector>

// H copies each of colors 1..C, uniformly shuffled.
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

// Confirms a sequence is exactly H copies each of 1..C -- catches a broken RNG or a
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

#endif // BALLSORT_INSTANCE_GEN_H
