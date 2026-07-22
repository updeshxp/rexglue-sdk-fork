// rexglue - one-shot raw PCM player.
//
// A minimal counterpart to WmaPlayer (see wma_player.h) for callers that
// already have decoded PCM in hand (no ASF/WMA container, no codec) and just
// want it to come out of the same audio backend/output device the rest of
// the engine uses -- e.g. a mod playing a bundled sound effect -- instead of
// reaching for a platform-specific API (winmm PlaySound on Windows has no
// equivalent on Linux, doesn't share the engine's output device selection,
// and ignores the audio_mute cvar).
//
// This still opens its own SDL audio stream, independent of the guest's own
// XAudio output, the same way WmaPlayer's music playback is independent of
// it -- there is no supported way for host-side code to inject samples into
// the guest XMA/XAudio client pipeline itself (that pipeline is driven by
// the guest's own callback-based frame submission protocol). "Own audio
// pipeline" here means the engine's shared SDL audio backend and its
// audio_mute/volume handling, not literally the guest's XAudio path.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

struct SDL_AudioStream;

namespace rex::audio {

class RawPcmPlayer {
 public:
  RawPcmPlayer();
  ~RawPcmPlayer();

  RawPcmPlayer(const RawPcmPlayer&) = delete;
  RawPcmPlayer& operator=(const RawPcmPlayer&) = delete;

  // Plays back |pcm| (interleaved, native-endian) once, replacing any
  // current playback on this instance. Returns false if the audio stream
  // couldn't be opened/configured (SDL not available, no output device).
  bool PlayInt16(std::vector<int16_t> pcm, uint32_t sample_rate, uint16_t channels);

  void Stop();
  void SetVolume(float volume);

 private:
  SDL_AudioStream* stream_ = nullptr;
  bool sdl_audio_inited_ = false;
  std::atomic<float> volume_{1.0f};
};

}  // namespace rex::audio
