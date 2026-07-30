// run_experiment -- run one search algorithm on one instance file and emit one CSV row.
//
// Deliberately one (algorithm, instance) pair per process. That is what makes the sweep a
// SLURM job array: each array task is one row, tasks are independent, a task that blows its
// time or memory limit is killed by SLURM without taking anything else with it, and a
// killed task is a *result* (a timeout) rather than a lost run. Aggregation happens
// afterwards over the collected rows -- see slurm/aggregate_results.py.
//
// Usage:
//   run_experiment --algorithm NAME --instance FILE [--repeats R] [--timing-repeats R]
//                  [--seed-label S] [--header]
//
// Metrics recorded, per the agreed set:
//
//   the paper's four (their Figure 6)         our additions
//   ----------------------------------        ---------------------------------
//   runtime_seconds                           nodes_expanded
//   peak_rss_kb                               nodes_generated
//   max_elements_in_memory                    solved (coverage / timeouts)
//   solution_length (their "needed moves")    suboptimality_ratio
//
// On determinism and repeats. Every algorithm here is deterministic: same instance, same
// algorithm, byte-identical node counts, every time. So node counts are measured once and
// averaging them would be a no-op. Wall-clock is the only noisy quantity, so *that* is what
// --timing-repeats repeats, and the median is kept (median, not mean: scheduler noise is
// one-sided and a single descheduled run should not move the number). Variation across
// instances comes from the instance-set seeds, not from re-running a fixed instance -- see
// the README.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#include "hog2_prelude.h" // must precede all HOG2 headers

#include "SearchEnvironment.h"
#include "BFS.h"
#include "UnitCostBidirectionalBFS.h"
#include "IDAStar.h"
#include "TemplateAStar.h"

#include "BallSort.h"
#include "dfvs_bound.h"
#include "frontier_bfs.h"
#include "iddfs.h"
#include "instance_gen.h"
#include "rscbt.h"

namespace {

// What one run produced. -1 / 0 mean "not applicable or not reached".
struct RunRecord {
	std::string algorithm;
	std::string cell;
	int numColors = 0;
	int tubeHeight = 0;
	int instanceIndex = -1;
	std::string seedLabel;

	bool solved = false;
	int solutionLength = -1;
	uint64_t nodesExpanded = 0;
	uint64_t nodesGenerated = 0;
	uint64_t maxElementsInMemory = 0;
	double runtimeSeconds = 0.0;
	long peakRssKb = 0;
	int iterations = 0;
	int rootLowerBound = -1;

	// Where this ran. Recorded because runtime_seconds is only comparable across rows that
	// ran on the same hardware; the sweep pins itself to one machine type, and these two
	// columns are what let that be checked after the fact rather than trusted.
	std::string node;
	std::string cpuModel;
};

// Hostname, short form.
std::string Hostname()
{
	char buffer[256] = {0};
	if (gethostname(buffer, sizeof(buffer)-1) != 0)
		return "unknown";
	std::string name(buffer);
	size_t dot = name.find('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

// CPU model from /proc/cpuinfo. Commas are stripped so the field cannot break the CSV.
std::string CpuModel()
{
	std::ifstream info("/proc/cpuinfo");
	std::string line;
	while (std::getline(info, line))
	{
		if (line.rfind("model name", 0) != 0)
			continue;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			break;
		std::string model = line.substr(colon+1);
		size_t first = model.find_first_not_of(" \t");
		size_t last = model.find_last_not_of(" \t\r\n");
		if (first == std::string::npos)
			break;
		model = model.substr(first, last-first+1);
		for (char &c : model)
			if (c == ',')
				c = ' ';
		return model;
	}
	return "unknown";
}

double NowSeconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec+ts.tv_nsec*1e-9;
}

// Peak resident set size of this process, which is the paper's Figure 6b metric. ru_maxrss
// is a high-water mark, so it is only meaningful for a process that ran one algorithm --
// another reason for one pair per process.
long PeakRssKb()
{
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return 0;
	return usage.ru_maxrss;   // kilobytes on Linux
}

double Median(std::vector<double> values)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	size_t mid = values.size()/2;
	if (values.size()%2 == 1)
		return values[mid];
	return 0.5*(values[mid-1]+values[mid]);
}

// ---------------------------------------------------------------------------
// The algorithm registry
// ---------------------------------------------------------------------------
//
// Each entry runs one algorithm once and fills in the metrics it can. Optimal algorithms
// must all agree on solution_length -- that is the correctness contract from CLAUDE.md, and
// the aggregation script checks it across rows rather than here, since one process only
// ever sees one algorithm.

template <int C, int H>
bool RunOne(const std::string &algorithm, const BallSortState<C, H> &start, RunRecord &out)
{
	typedef BallSortState<C, H> State;
	BallSort<C, H> env;
	State goal = env.GetGoalState();
	std::vector<State> path;

	BallSortDFVSHeuristic<C, H> paperBound(&env);
	out.rootLowerBound = static_cast<int>(paperBound.HCost(start, goal));

	if (algorithm == "bfs")
	{
		BFS<State, BallSortMove, BallSort<C, H>> bfs;
		bfs.SetVerbose(false);
		bfs.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = bfs.GetNodesExpanded();
		out.nodesGenerated = bfs.GetNodesTouched();
	}
	else if (algorithm == "bibfs")
	{
		UnitCostBidirectionalBFS<State, BallSortMove> biBfs;
		biBfs.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = biBfs.GetNodesExpanded();
		out.nodesGenerated = biBfs.GetNodesTouched();
	}
	else if (algorithm == "frontier-bfs")
	{
		auto r = FrontierBFSSolve(env, start, goal);
		out.solutionLength = r.solutionLength;
		out.nodesExpanded = r.nodesExpanded;
		out.nodesGenerated = r.nodesExpanded;
	}
	else if (algorithm == "iddfs")
	{
		auto r = IDDFSSolve(env, start, goal);
		out.solutionLength = r.solutionLength;
		out.nodesExpanded = r.nodesExpanded;
		out.nodesGenerated = r.nodesExpanded;
	}
	else if (algorithm == "dijkstra")
	{
		ZeroHeuristic<State> zero;
		TemplateAStar<State, BallSortMove, BallSort<C, H>> search;
		search.SetHeuristic(&zero);
		search.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = search.GetNodesExpanded();
		out.nodesGenerated = search.GetNodesTouched();
	}
	else if (algorithm == "astar-misplaced" || algorithm == "astar-paper")
	{
		TemplateAStar<State, BallSortMove, BallSort<C, H>> search;
		// astar-misplaced leaves the heuristic unset, so IDAStar/TemplateAStar fall back to
		// the environment itself (BallSort::HCost, the misplaced-ball count).
		if (algorithm == "astar-paper")
			search.SetHeuristic(&paperBound);
		search.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = search.GetNodesExpanded();
		out.nodesGenerated = search.GetNodesTouched();
	}
	else if (algorithm == "idastar-misplaced" || algorithm == "idastar-paper")
	{
		IDAStar<State, BallSortMove, false> search;
		if (algorithm == "idastar-paper")
			search.SetHeuristic(&paperBound);
		search.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = search.GetNodesExpanded();
		out.nodesGenerated = search.GetNodesTouched();
	}
	else if (algorithm.rfind("wastar-", 0) == 0)
	{
		// wastar-<weight>, e.g. wastar-1.5, wastar-2, wastar-5.
		double weight = atof(algorithm.c_str()+strlen("wastar-"));
		if (weight <= 0)
			return false;
		TemplateAStar<State, BallSortMove, BallSort<C, H>> search;
		search.SetHeuristic(&paperBound);
		search.SetWeight(weight);
		search.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = search.GetNodesExpanded();
		out.nodesGenerated = search.GetNodesTouched();
	}
	else if (algorithm == "greedy")
	{
		TemplateAStar<State, BallSortMove, BallSort<C, H>> search;
		search.SetHeuristic(&paperBound);
		search.SetPhi([](double h, double g) { return h; });   // order on h alone
		search.GetPath(&env, start, goal, path);
		out.solutionLength = static_cast<int>(path.size())-1;
		out.nodesExpanded = search.GetNodesExpanded();
		out.nodesGenerated = search.GetNodesTouched();
	}
	else if (algorithm == "rscbt" || algorithm == "rscbt-nosym")
	{
		// Their algorithm. rscbt is the faithful configuration, with the color-permutation
		// symmetry reduction of their §6; rscbt-nosym turns it off so the node counts are
		// comparable with the algorithms that search the raw state space.
		auto r = RSCBTSolve<C, H>(env, start, algorithm == "rscbt");
		out.solutionLength = r.solutionLength;
		out.nodesExpanded = r.nodesExpanded;
		out.nodesGenerated = r.nodesGenerated;
		out.maxElementsInMemory = r.maxElements;
		out.iterations = r.iterations;
	}
	else
	{
		return false;
	}

	out.solved = out.solutionLength > 0
				 || (out.solutionLength == 0 && env.GoalTest(start));
	return true;
}

// Dispatch a (numColors, tubeHeight) pair read at runtime to the compile-time template.
// BallSort is templated on both, so every cell we can run has to be instantiated here; the
// list is exactly the paper's grid intersected with what the uint64 state hash can encode
// (see the README). A cell outside it is a clean error rather than a silent wrong answer.
#define BALLSORT_CELLS(X) \
	X(2, 2) X(2, 3) X(2, 4) X(2, 5) X(2, 6) X(2, 7) X(2, 8) X(2, 9) X(2, 10) X(2, 11) X(2, 12) X(2, 13) \
	X(3, 2) X(3, 3) X(3, 4) X(3, 5) X(3, 6) X(3, 7) \
	X(4, 2) X(4, 3) X(4, 4) X(4, 5) \
	X(5, 2) X(5, 3) X(5, 4) \
	X(6, 2) X(6, 3) \
	X(7, 2) \
	X(8, 2)

bool Dispatch(const std::string &algorithm, const std::vector<int> &colors,
			  int numColors, int tubeHeight, RunRecord &out)
{
#define BALLSORT_TRY(C, H) \
	if (numColors == (C) && tubeHeight == (H)) \
	{ \
		BallSortState<C, H> start; \
		start.SetFromColorSequence(colors); \
		return RunOne<C, H>(algorithm, start, out); \
	}
	BALLSORT_CELLS(BALLSORT_TRY)
#undef BALLSORT_TRY
	fprintf(stderr, "error: cell %dx%d (colors=%d height=%d) is not instantiated; "
					"either the uint64 state hash cannot encode it or it needs adding to "
					"BALLSORT_CELLS in run_experiment.cpp\n",
			tubeHeight, numColors+1, numColors, tubeHeight);
	return false;
}

void PrintHeader()
{
	printf("algorithm,cell,colors,height,instance,seed_label,solved,solution_length,"
		   "nodes_expanded,nodes_generated,max_elements_in_memory,runtime_seconds,"
		   "peak_rss_kb,iterations,root_lower_bound,node,cpu_model\n");
}

void PrintRow(const RunRecord &r)
{
	printf("%s,%s,%d,%d,%d,%s,%d,%d,%llu,%llu,%llu,%.6f,%ld,%d,%d,%s,%s\n",
		   r.algorithm.c_str(), r.cell.c_str(), r.numColors, r.tubeHeight,
		   r.instanceIndex, r.seedLabel.c_str(), r.solved ? 1 : 0, r.solutionLength,
		   (unsigned long long)r.nodesExpanded, (unsigned long long)r.nodesGenerated,
		   (unsigned long long)r.maxElementsInMemory, r.runtimeSeconds,
		   r.peakRssKb, r.iterations, r.rootLowerBound,
		   r.node.c_str(), r.cpuModel.c_str());
}

void PrintUsage(const char *prog)
{
	fprintf(stderr,
			"usage: %s --algorithm NAME --instance FILE [options]\n"
			"\n"
			"algorithms:\n"
			"  bfs, bibfs, frontier-bfs, iddfs, dijkstra   uninformed / zero-heuristic\n"
			"  astar-misplaced, idastar-misplaced          with BallSort::HCost\n"
			"  astar-paper, idastar-paper                  with the paper's DFVS bound\n"
			"  wastar-<w>, greedy                          suboptimal, e.g. wastar-1.5\n"
			"  rscbt, rscbt-nosym                          the paper's own algorithm\n"
			"\n"
			"options:\n"
			"  --timing-repeats R  repeat the run R times and keep the median wall-clock\n"
			"                      (default 3). Node counts are deterministic and are taken\n"
			"                      from the first run.\n"
			"  --seed-label S      tag the row with the instance-set seed it came from\n"
			"  --header            print the CSV header before the row\n",
			prog);
}

} // namespace

int main(int argc, char **argv)
{
	std::string algorithm, instancePath, seedLabel;
	int timingRepeats = 3;
	bool wantHeader = false;

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

		if (arg == "--algorithm")      algorithm = nextArg("--algorithm");
		else if (arg == "--instance")  instancePath = nextArg("--instance");
		else if (arg == "--seed-label") seedLabel = nextArg("--seed-label");
		else if (arg == "--timing-repeats" || arg == "--repeats")
			timingRepeats = atoi(nextArg("--timing-repeats"));
		else if (arg == "--header")    wantHeader = true;
		else if (arg == "-h" || arg == "--help") { PrintUsage(argv[0]); return 0; }
		else
		{
			fprintf(stderr, "error: unrecognized argument '%s'\n", arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		}
	}

	if (algorithm.empty() || instancePath.empty())
	{
		fprintf(stderr, "error: --algorithm and --instance are both required\n");
		PrintUsage(argv[0]);
		return 2;
	}
	if (timingRepeats < 1)
		timingRepeats = 1;

	std::ifstream in(instancePath);
	if (!in)
	{
		fprintf(stderr, "error: could not open '%s'\n", instancePath.c_str());
		return 1;
	}
	int numColors = -1, tubeHeight = -1;
	std::string error;
	std::vector<int> colors = ReadPaperInstance(in, numColors, tubeHeight, error);
	if (!error.empty())
	{
		fprintf(stderr, "error: %s: %s\n", instancePath.c_str(), error.c_str());
		return 1;
	}

	RunRecord record;
	record.algorithm = algorithm;
	record.cell = std::to_string(tubeHeight)+"x"+std::to_string(numColors+1);
	record.numColors = numColors;
	record.tubeHeight = tubeHeight;
	record.seedLabel = seedLabel;
	// Instance index off the file name: ..._<index>.in, matching the generator's naming.
	{
		size_t underscore = instancePath.rfind('_');
		size_t dot = instancePath.rfind(".in");
		if (underscore != std::string::npos && dot != std::string::npos && dot > underscore)
			record.instanceIndex = atoi(instancePath.substr(underscore+1, dot-underscore-1).c_str());
	}

	std::vector<double> times;
	for (int r = 0; r < timingRepeats; r++)
	{
		RunRecord attempt = record;
		double t0 = NowSeconds();
		if (!Dispatch(algorithm, colors, numColors, tubeHeight, attempt))
		{
			fprintf(stderr, "error: unknown algorithm '%s' or uninstantiated cell\n", algorithm.c_str());
			return 2;
		}
		times.push_back(NowSeconds()-t0);
		if (r == 0)
		{
			// Node counts are deterministic, so the first run settles them; only the clock
			// gets repeated.
			attempt.seedLabel = record.seedLabel;
			attempt.instanceIndex = record.instanceIndex;
			record = attempt;
		}
	}

	record.runtimeSeconds = Median(times);
	record.peakRssKb = PeakRssKb();
	record.node = Hostname();
	record.cpuModel = CpuModel();

	if (wantHeader)
		PrintHeader();
	PrintRow(record);
	return 0;
}
