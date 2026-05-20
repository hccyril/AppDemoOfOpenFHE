#pragma once

#include <map>
#include <set>
#include <vector>
#include <omp.h>

// #include "FHEUtils.h" // Only depends on OpenFHE, not FHEUtils
// OpenFHE headers
#include "pke/openfhe.h"
#include "binfhe/binfhecontext.h"

#ifndef LOG2
#define LOG2(n) (31U - __builtin_clz(n))
#define LOG2LL(n) (63U - __builtin_clzll(n))
#define LOG2_CEIL(n) ((n) <= 1 ? 0U : 32U - __builtin_clz((n) - 1))
#endif

// This macro is used to enable time block for analyzing the program's specific time consumption
#define TIME_CHECK

using namespace lbcrypto;
