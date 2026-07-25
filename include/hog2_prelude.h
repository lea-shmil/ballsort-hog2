// hog2_prelude.h -- include this BEFORE any HOG2 header.
//
// HOG2 (PDB-refactor) was written against an older libstdc++ that pulled several
// standard headers in transitively. gcc 15 no longer does, so some HOG2 headers
// reference std:: types they never include. We fix that here rather than patching
// the vendored submodule, which keeps ./hog2 a clean upstream checkout.
//
// The list below is the empirically determined minimum for the headers this
// project uses: SearchEnvironment.h, BFS.h, FrontierBFS.h,
// UnitCostBidirectionalBFS.h, DFID.h, IDAStar.h, TemplateAStar.h.
// Verified with -fsyntax-only against gcc 15.2.0: with <deque> they compile
// clean; without it, 32 errors (BFS.h uses std::deque for its open list).
//
// If a newly included HOG2 header produces "'X' is not a member of 'std'",
// add the header gcc names in its "is defined in header" note here.

#ifndef BALLSORT_HOG2_PRELUDE_H
#define BALLSORT_HOG2_PRELUDE_H

#include <deque>

#endif // BALLSORT_HOG2_PRELUDE_H
