#pragma once

#include <limits>

// These options invalidate finite-input checks or the specified arithmetic.
// Reject them in every translation unit that consumes an inline core function.
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0) || \
    defined(__ASSOCIATIVE_MATH__) || defined(__RECIPROCAL_MATH__) || \
    defined(__NO_SIGNED_ZEROS__) || defined(_M_FP_FAST)
#error "SWEGCA core requires strict floating-point arithmetic; unsafe math options are unsupported"
#endif

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53,
              "SWEGCA evidence arithmetic requires IEEE-754 binary64");

// Consumers must also use -ffp-contract=off and retain the default rounding
// mode and subnormals. The supported build applies that flag to all callers.
// This header adds no runtime operation, allocation, or mutable state.
