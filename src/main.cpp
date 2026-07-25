// Harness smoke test: confirms the headless HOG2 include set compiles and links.
// No domain yet -- environments/BallSort.h comes next.

#include <cstdio>

#include "hog2_prelude.h" // must precede all HOG2 headers

#include "SearchEnvironment.h"
#include "BFS.h"
#include "TemplateAStar.h"

int main()
{
	printf("OK: ballsort-hog2 harness built against headless HOG2\n");
	return 0;
}
