#pragma once

#include <cmath>

// GCC 9 with the ChCore musl headers exposes these functions only in std,
// while ggml uses the unqualified C spellings in C++ translation units.
using std::isinf;
using std::isnan;
