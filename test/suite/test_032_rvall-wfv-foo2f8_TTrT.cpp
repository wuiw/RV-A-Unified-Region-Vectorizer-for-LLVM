#include <cmath>

extern "C" bool rv_all(bool);

extern "C" float
foo(float a, float b)
{
  return rv_all(fabs(a) >= -fabs(b)) ? a : b;
}
