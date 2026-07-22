// rexglue - shared audio ducking factor. See ducking.h.

#include <rex/audio/ducking.h>

#include <algorithm>
#include <atomic>

namespace rex::audio {

namespace {
std::atomic<float> g_duck_factor{1.0f};
}

void SetDuckFactor(float factor) {
  g_duck_factor.store(std::clamp(factor, 0.0f, 1.0f));
}

float GetDuckFactor() {
  return g_duck_factor.load();
}

}  // namespace rex::audio
