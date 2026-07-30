// generate_instances -- writes a reproducible set of BallSort start instances.
//
// ATTRIBUTION. The benchmark design this tool implements -- the generation procedure, the
// file format, the {h}x{T} cell and file naming, the directory layout, the parameter grid
// and the ten-instances-per-cell protocol -- is from Althaus et al., SoCS 2025, and is
// reimplemented here rather than copied. See the attribution block at the top of
// instance_gen.h and the "Provenance and attribution" section of the README for the
// citation and the item-by-item breakdown of what is theirs and what is ours.
//
// Independent of HOG2 and of BallSort.h on purpose: an instance is nothing but a
// permutation of colors, and generating it doesn't need the search machinery. The
// experiment driver that reads these back and actually runs the algorithms is separate
// (see README) -- this tool only has to produce the instances and get that part right,
// which is why it stays this small. instance_gen.h documents what the paper does and
// where we deviate.
//
// Two modes:
//
//   Grid mode (the one the experiment set comes from)
//     generate_instances --grid core|runnable|paper --outdir DIR [--count N] [--seed S]
//
//   writes DIR/{h}x{T}/random_generated_{h}x{T}_{i}.in, the paper's own layout and file
//   format, plus DIR/manifest.csv describing every cell and instance. Cells are named
//   the paper's way, by tube height and *total* tubes with the reserve counted.
//
//   Single-cell mode (kept for one-off tests)
//     generate_instances --height H (--colors C | --tubes T) --count N --seed S
//                        [--out FILE] [--format paper|flat]
//
//   `--format flat` writes one instance per line to a single file (or stdout) --
//   convenient to eyeball, and the format this tool wrote before it grew grid mode.
//
// Reproducibility is the point: the same --seed and --count reproduce the same set
// byte-for-byte, and because each instance is seeded from (seed, h, T, index) rather
// than from a running stream, raising --count leaves the earlier instances untouched and
// each cell can be regenerated on its own.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "instance_gen.h"

namespace {

const int kDefaultCount = 10;      // the paper's ten instances per cell
const long kDefaultSeed = 20250726;

void PrintUsage(const char *prog)
{
	fprintf(stderr,
			"usage: %s --grid core|runnable|paper --outdir DIR [--count N] [--seed S] [--min-lb L]\n"
			"       %s --height H (--colors C | --tubes T) --count N --seed S\n"
			"               [--out FILE] [--format paper|flat] [--min-lb L]\n"
			"\n"
			"grid mode -- writes the paper's directory layout and file format:\n"
			"  --grid G     which cell set (default core):\n"
			"                 paper    -- all 53 cells of the published benchmark grid\n"
			"                 runnable -- paper cells our uint64 state hash can encode\n"
			"                 core     -- runnable cells minus the trivial and out-of-reach tiers\n"
			"  --outdir DIR output directory (required); DIR/{h}x{T}/ per cell, plus manifest.csv\n"
			"  --count N    instances per cell (default %d, the paper's)\n"
			"  --seed S     master seed (default %ld)\n"
			"\n"
			"single-cell mode:\n"
			"  --height H   balls per tube (required)\n"
			"  --colors C   number of colors / colored tubes\n"
			"  --tubes T    total tubes including the reserve, i.e. C+1 (the paper's indexing)\n"
			"  --count N    number of instances (required)\n"
			"  --seed S     master seed (required)\n"
			"  --out FILE   output path (default: stdout)\n"
			"  --format F   paper (one file per instance) or flat (one line per instance)\n"
			"\n"
			"both modes:\n"
			"  --min-lb L   reject a drawn arrangement whose simple lower bound is below L\n"
			"               (default 1, which rejects only an already-solved start; 0 is the\n"
			"               paper's unfiltered shuffle)\n"
			"\n"
			"inspection:\n"
			"  --read FILE... parse instance files and report cell, simple lower bound and\n"
			"               balls already in final position. Accepts the paper's own\n"
			"               resources/paper_inputs/*.in unchanged -- the format is theirs.\n",
			prog, prog, kDefaultCount, kDefaultSeed);
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

// mkdir -p semantics. Single-level mkdir is not enough: the multi-seed sweep generates into
// nested paths like instances/seed-20250726/, and the parent may not exist yet.
bool MakeDir(const std::string &path)
{
	auto makeOne = [](const std::string &dir) {
		if (dir.empty() || dir == ".")
			return true;
		if (mkdir(dir.c_str(), 0755) == 0)
			return true;
		struct stat st;
		return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
	};

	for (size_t i = 1; i < path.size(); i++)
	{
		if (path[i] == '/' && !makeOne(path.substr(0, i)))
			return false;
	}
	return makeOne(path);
}

// Draw instance `index` of a cell. Rejection is only ever triggered by --min-lb; with the
// default of 1 that means "redraw an arrangement that is already the goal", which can
// only happen in the smallest cells (with h=2 and two colors, 1 shuffle in 6 is the
// goal). Each retry perturbs the index so the redraw is still a pure function of
// (seed, h, T, index) and stays reproducible.
bool DrawInstance(uint64_t masterSeed, int numColors, int tubeHeight, int numTubes,
				  int index, int minLowerBound, std::vector<int> &colorsOut)
{
	const int kMaxAttempts = 1000;
	for (int attempt = 0; attempt < kMaxAttempts; attempt++)
	{
		std::mt19937 rng(static_cast<std::mt19937::result_type>(
				InstanceSeed(masterSeed, tubeHeight, numTubes, index+attempt*1000003)));
		std::vector<int> colors = RandomColorSequence(numColors, tubeHeight, rng);
		if (!VerifyColorSequence(colors, numColors, tubeHeight))
			return false;
		if (SimpleLowerBound(colors, numColors, tubeHeight) >= minLowerBound)
		{
			colorsOut = colors;
			return true;
		}
	}
	return false;
}

void WriteFlatHeader(std::ostream &out, int numColors, int tubeHeight, int count, long seed)
{
	out << "# BallSort instances\n";
	out << "# format: each non-comment line is C*H space-separated colors (1..C),\n";
	out << "# read as tube 1 bottom..top, tube 2 bottom..top, ..., tube C bottom..top;\n";
	out << "# reserve tube 0 is implicitly empty (the domain's start state)\n";
	out << "# colors=" << numColors << " height=" << tubeHeight
		<< " tubes=" << numColors+1
		<< " count=" << count << " seed=" << seed << "\n";
}

int RunGridMode(const std::string &gridName, const std::string &outDir,
				int count, long seed, int minLowerBound)
{
	std::vector<GridCell> cells;
	if (gridName == "paper")
		cells = PaperGridCells();
	else if (gridName == "runnable")
		cells = RunnableGridCells();
	else if (gridName == "core")
		cells = CoreGridCells();
	else
	{
		fprintf(stderr, "error: --grid must be one of core, runnable, paper\n");
		return 2;
	}

	if (!MakeDir(outDir))
	{
		fprintf(stderr, "error: could not create directory '%s'\n", outDir.c_str());
		return 1;
	}

	const std::string manifestPath = outDir+"/manifest.csv";
	std::ofstream manifest(manifestPath);
	if (!manifest)
	{
		fprintf(stderr, "error: could not open '%s' for writing\n", manifestPath.c_str());
		return 1;
	}
	manifest << "cell,height,tubes,colors,balls,reachable_states,hash_fits_uint64,tier,"
				"instance,file,simple_lower_bound,balls_in_final_position\n";

	int written = 0;
	for (const GridCell &cell : cells)
	{
		const int numColors = cell.NumColors();
		const int tubeHeight = cell.tubeHeight;
		const std::string cellDir = outDir+"/"+cell.Name();
		if (!MakeDir(cellDir))
		{
			fprintf(stderr, "error: could not create directory '%s'\n", cellDir.c_str());
			return 1;
		}

		const long double states = ReachableStateCount(numColors, tubeHeight);
		const bool fits = StateHashFitsUint64(numColors, tubeHeight);
		const char *tier = CellTierName(CellTierOf(cell));

		for (int i = 0; i < count; i++)
		{
			std::vector<int> colors;
			if (!DrawInstance(static_cast<uint64_t>(seed), numColors, tubeHeight,
							  cell.numTubes, i, minLowerBound, colors))
			{
				fprintf(stderr, "error: could not draw instance %d of cell %s "
								"satisfying --min-lb %d\n", i, cell.Name().c_str(), minLowerBound);
				return 1;
			}

			const std::string fileName = "random_generated_"+cell.Name()+"_"+std::to_string(i)+".in";
			const std::string filePath = cellDir+"/"+fileName;
			std::ofstream f(filePath);
			if (!f)
			{
				fprintf(stderr, "error: could not open '%s' for writing\n", filePath.c_str());
				return 1;
			}
			WritePaperInstance(f, colors, numColors, tubeHeight);
			f.close();

			// Read back what we just wrote. A silently corrupt or misformatted instance
			// file is the one bug in this tool that would only surface after a cluster
			// run, so the writer is never trusted on its own -- same reasoning as the
			// VerifyColorSequence call in DrawInstance.
			std::ifstream back(filePath);
			int readColors = -1, readHeight = -1;
			std::string readError;
			std::vector<int> reread = ReadPaperInstance(back, readColors, readHeight, readError);
			if (!readError.empty() || readColors != numColors || readHeight != tubeHeight
				|| reread != colors)
			{
				fprintf(stderr, "internal error: '%s' did not read back as written (%s)\n",
						filePath.c_str(),
						readError.empty() ? "content mismatch" : readError.c_str());
				return 1;
			}

			manifest << cell.Name() << "," << tubeHeight << "," << cell.numTubes << ","
					 << numColors << "," << numColors*tubeHeight << ","
					 << static_cast<double>(states) << "," << (fits ? 1 : 0) << ","
					 << tier << "," << i << "," << cell.Name()+"/"+fileName << ","
					 << SimpleLowerBound(colors, numColors, tubeHeight) << ","
					 << BallsInFinalPosition(colors, numColors, tubeHeight) << "\n";
			written++;
		}
	}

	fprintf(stderr, "wrote %d instances over %zu cells (grid=%s count=%d seed=%ld) to %s\n",
			written, cells.size(), gridName.c_str(), count, seed, outDir.c_str());
	fprintf(stderr, "manifest: %s\n", manifestPath.c_str());
	return 0;
}

// Parse an existing instance file and report what it is. Two uses: confirming a file this
// tool wrote is what we think it is, and confirming we can ingest the paper's own
// published resources/paper_inputs/*.in unchanged -- the format is theirs, so their files
// are valid input here.
int RunReadMode(const std::vector<std::string> &paths)
{
	int bad = 0;
	for (const std::string &path : paths)
	{
		std::ifstream in(path);
		if (!in)
		{
			fprintf(stderr, "%s: could not open\n", path.c_str());
			bad++;
			continue;
		}
		int numColors = -1, tubeHeight = -1;
		std::string error;
		std::vector<int> colors = ReadPaperInstance(in, numColors, tubeHeight, error);
		if (!error.empty())
		{
			fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
			bad++;
			continue;
		}
		printf("%s: cell %dx%d (colors=%d height=%d balls=%d) "
			   "simple_lower_bound=%d balls_in_final_position=%d\n",
			   path.c_str(), tubeHeight, numColors+1, numColors, tubeHeight,
			   numColors*tubeHeight,
			   SimpleLowerBound(colors, numColors, tubeHeight),
			   BallsInFinalPosition(colors, numColors, tubeHeight));
	}
	return bad == 0 ? 0 : 1;
}

int RunSingleCellMode(int numColors, int tubeHeight, int count, long seed,
					  const std::string &outPath, const std::string &format, int minLowerBound)
{
	const int numTubes = numColors+1;

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

	if (format == "flat")
		WriteFlatHeader(out, numColors, tubeHeight, count, seed);

	for (int i = 0; i < count; i++)
	{
		std::vector<int> colors;
		if (!DrawInstance(static_cast<uint64_t>(seed), numColors, tubeHeight, numTubes,
						  i, minLowerBound, colors))
		{
			fprintf(stderr, "error: could not draw instance %d satisfying --min-lb %d\n",
					i, minLowerBound);
			return 1;
		}

		if (format == "flat")
		{
			for (size_t k = 0; k < colors.size(); k++)
				out << (k ? " " : "") << colors[k];
			out << "\n";
		}
		else
		{
			if (count > 1)
				out << "# instance " << i << "\n";
			WritePaperInstance(out, colors, numColors, tubeHeight);
		}
	}

	if (!outPath.empty())
		fprintf(stderr, "wrote %d instances (height=%d tubes=%d colors=%d seed=%ld) to %s\n",
				count, tubeHeight, numTubes, numColors, seed, outPath.c_str());
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	std::string gridName, outDir, outPath, format = "paper";
	int numColors = -1, tubeHeight = -1, count = -1, minLowerBound = 1;
	long seed = kDefaultSeed;
	bool haveSeed = false, haveCount = false, readMode = false;
	std::vector<std::string> readPaths;

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

		if (arg == "--read")
		{
			readMode = true;
		}
		else if (arg == "--grid")
		{
			gridName = nextArg("--grid");
		}
		else if (arg == "--outdir")
		{
			outDir = nextArg("--outdir");
		}
		else if (arg == "--colors")
		{
			if (!ParseInt(nextArg("--colors"), numColors)) { fprintf(stderr, "error: --colors needs an integer\n"); return 2; }
		}
		else if (arg == "--tubes")
		{
			int numTubes = -1;
			if (!ParseInt(nextArg("--tubes"), numTubes)) { fprintf(stderr, "error: --tubes needs an integer\n"); return 2; }
			if (numTubes < 2) { fprintf(stderr, "error: --tubes counts the reserve, so it must be at least 2\n"); return 2; }
			numColors = numTubes-1;
		}
		else if (arg == "--height")
		{
			if (!ParseInt(nextArg("--height"), tubeHeight)) { fprintf(stderr, "error: --height needs an integer\n"); return 2; }
		}
		else if (arg == "--count")
		{
			if (!ParseInt(nextArg("--count"), count)) { fprintf(stderr, "error: --count needs an integer\n"); return 2; }
			haveCount = true;
		}
		else if (arg == "--min-lb")
		{
			if (!ParseInt(nextArg("--min-lb"), minLowerBound)) { fprintf(stderr, "error: --min-lb needs an integer\n"); return 2; }
			if (minLowerBound < 0) { fprintf(stderr, "error: --min-lb must be non-negative\n"); return 2; }
		}
		else if (arg == "--format")
		{
			format = nextArg("--format");
			if (format != "paper" && format != "flat")
			{
				fprintf(stderr, "error: --format must be paper or flat\n");
				return 2;
			}
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
		else if (readMode && arg.rfind("--", 0) != 0)
		{
			readPaths.push_back(arg);
		}
		else
		{
			fprintf(stderr, "error: unrecognized argument '%s'\n", arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		}
	}

	if (readMode)
	{
		if (readPaths.empty())
		{
			fprintf(stderr, "error: --read needs at least one instance file\n");
			return 2;
		}
		return RunReadMode(readPaths);
	}

	const bool gridMode = !gridName.empty() || !outDir.empty();

	if (gridMode)
	{
		if (gridName.empty()) gridName = "core";
		if (outDir.empty())
		{
			fprintf(stderr, "error: grid mode needs --outdir\n");
			PrintUsage(argv[0]);
			return 2;
		}
		if (numColors > 0 || tubeHeight > 0 || !outPath.empty())
		{
			fprintf(stderr, "error: --colors/--tubes/--height/--out are single-cell options; "
							"grid mode takes its cells from --grid\n");
			return 2;
		}
		if (!haveCount) count = kDefaultCount;
		if (count < 1)
		{
			fprintf(stderr, "error: --count must be positive\n");
			return 2;
		}
		return RunGridMode(gridName, outDir, count, seed, minLowerBound);
	}

	if (numColors < 1 || tubeHeight < 1 || count < 1 || !haveSeed)
	{
		fprintf(stderr, "error: single-cell mode needs --height, --colors or --tubes, "
						"--count and --seed, all positive\n");
		PrintUsage(argv[0]);
		return 2;
	}
	return RunSingleCellMode(numColors, tubeHeight, count, seed, outPath, format, minLowerBound);
}
