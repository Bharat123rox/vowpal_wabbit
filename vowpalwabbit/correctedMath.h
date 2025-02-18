// On Windows, exp(-infinity) incorrectly returns -infinity instead of 0.
// So we replace it with our own version that checks for this condition.

#pragma once

#include <cmath>
#include <cmath>

#ifdef _WIN32
// this is a bug in VS2013, fixed in VS2015 runtime
template <typename T>
T correctedExp(T exponent)
{
  if (isinf(exponent) && exponent < T(0))
  {
    return T(0);
  }
  else
  {
    return exp(exponent);
  }
}
#else
// std::exp is used because on Linux, not using the namespace caused a different implementation of
// exp to be linked providing incorrect values when `#include <boost/program_options.hpp>` was
// removed in global_data.h
#define correctedExp std::exp
#endif
