// inputkit - a stick's radial dead zone (see stick.h): Big Red One's apply_dead_zone, the same float steps.

#include "inputkit/stick.h"

#include <cmath>

extern "C" void inputkit_radial_dead_zone(int* x, int* y, float dead_zone) {
  if (!(dead_zone > 0.0f)) return;
  if (dead_zone > 0.9f) dead_zone = 0.9f;
  const float fx = (float)*x / 32767.0f, fy = (float)*y / 32767.0f;
  const float length = std::sqrt(fx * fx + fy * fy);
  if (length <= dead_zone) {
    *x = *y = 0;
    return;
  }
  const float travel = length > 1.0f ? 1.0f : length;
  const float scale = (travel - dead_zone) / (1.0f - dead_zone) / length;
  *x = (int)(fx * scale * 32767.0f);
  *y = (int)(fy * scale * 32767.0f);
}
