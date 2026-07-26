// generate_instances -- writes a reproducible set of random BallSort start instances.
//
// Independent of HOG2 and of BallSort.h on purpose: an instance is nothing but a
// permutation of colors, and generating it doesn't need the search machinery. The
// experiment driver that reads this file back and actually runs the algorithms is
// separate, future work (see README) -- this tool only has to produce instances and
// get that part right, which is why it stays this small.
//
// Usage:
//   generate_instances --colors C --height H --count N --seed S [--out FILE]
//
// Output format (see WriteHeader for the authoritative version):
//   '#' comment lines carry colors/height/count/seed for the reader's benefit;
//   each following line is one instance: C*H space-separated colors, read as
//   tube 1 bottom..top, tube 2 bottom..top, ..., tube C bottom..top. Reserve
//   tube 0 is implicitly empty, per the domain's start-state definition.
//
// Same seed + same colors/height/count always produces the same file -- that
// reproducibility is the point, since the algorithm-comparison sweep needs every
// algorithm run against the identical instance set.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

#include "instance_gen.h"

namespace {

void PrintUsage(const char *prog)
{
	fprintf(stderr,
			"usage: %s --colors C --height H --count N --seed S [--out FILE]\n"
			"  --colors C   number of colors / colored tubes (required)\n"
			"  --height H   balls per tube (required)\n"
			"  --count N    number of instances to generate (required)\n"
			"  --seed S     RNG seed, for reproducibility (required)\n"
			"  --out FILE   output path (default: stdout)\n",
			prog);
}

bool ParseInt(const char *s, int &out)
{
	char *end = nullptr;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0')
		return false;
	out = static_cast<int>(v);
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	int numColors = -1, tubeHeight = -1, count = -1;
	long seed = -1;
	bool haveSeed = false;
	std::string outPath;

	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		auto nextArg = [&](const char *flag) -> const char* {
			if (i+1 >= argc)
			{
				fprintf(stderr, "error: %s needs a value\n", flag);
				exit(2);
			}
			return argv[++i];
		};

		if (arg == "--colors")
		{
			if (!ParseInt(nextArg("--colors"), numColors)) { fprintf(stderr, "error: --colors needs an integer\n"); return 2; }
		}
		else if (arg == "--height")
		{
			if (!ParseInt(nextArg("--height"), tubeHeight)) { fprintf(stderr, "error: --height needs an integer\n"); return 2; }
		}
		else if (arg == "--count")
		{
			if (!ParseInt(nextArg("--count"), count)) { fprintf(stderr, "error: --count needs an integer\n"); return 2; }
		}
		else if (arg == "--seed")
		{
			char *end = nullptr;
			const char *val = nextArg("--seed");
			seed = strtol(val, &end, 10);
			if (end == val || *end != '\0') { fprintf(stderr, "error: --seed needs an integer\n"); return 2; }
			haveSeed = true;
		}
		else if (arg == "--out")
		{
			outPath = nextArg("--out");
		}
		else if (arg == "-h" || arg == "--help")
		{
			PrintUsage(argv[0]);
			return 0;
		}
		else
		{
			fprintf(stderr, "error: unrecognized argument '%s'\n", arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		}
	}

	if (numColors < 1 || tubeHeight < 1 || count < 1 || !haveSeed)
	{
		fprintf(stderr, "error: --colors, --height, --count and --seed are all required and must be positive\n");
		PrintUsage(argv[0]);
		return 2;
	}

	std::ofstream fileOut;
	if (!outPath.empty())
	{
		fileOut.open(outPath);
		if (!fileOut)
		{
			fprintf(stderr, "error: could not open '%s' for writing\n", outPath.c_str());
			return 1;
		}
	}
	std::ostream &out = outPath.empty() ? std::cout : fileOut;

	out << "# BallSort instances\n";
	out << "# format: each non-comment line is C*H space-separated colors (1..C),\n";
	out << "# read as tube 1 bottom..top, tube 2 bottom..top, ..., tube C bottom..top;\n";
	out << "# reserve tube 0 is implicitly empty (the domain's start state)\n";
	out << "# colors=" << numColors << " height=" << tubeHeight
		<< " count=" << count << " seed=" << seed << "\n";

	std::mt19937 rng(static_cast<unsigned long>(seed));
	for (int i = 0; i < count; i++)
	{
		std::vector<int> colors = RandomColorSequence(numColors, tubeHeight, rng);
		if (!VerifyColorSequence(colors, numColors, tubeHeight))
		{
			fprintf(stderr, "internal error: generated instance %d failed verification -- aborting\n", i);
			return 1;
		}
		for (size_t k = 0; k < colors.size(); k++)
			out << (k ? " " : "") << colors[k];
		out << "\n";
	}

	if (!outPath.empty())
		fprintf(stderr, "wrote %d instances (colors=%d height=%d seed=%ld) to %s\n",
				count, numColors, tubeHeight, seed, outPath.c_str());

	return 0;
}
