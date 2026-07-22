// rexglue - one-shot raw PCM player. See include/rex/audio/raw_pcm_player.h.

#include <rex/audio/raw_pcm_player.h>

#include <rex/audio/flags.h>
#include <rex/logging.h>

#include <SDL3/SDL.h>

namespace rex::audio {

RawPcmPlayer::RawPcmPlayer() = default;

RawPcmPlayer::~RawPcmPlayer() {
  Stop();
  if (sdl_audio_inited_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_inited_ = false;
  }
}

bool RawPcmPlayer::PlayInt16(std::vector<int16_t> pcm, uint32_t sample_rate, uint16_t channels) {
  Stop();

  if (!sdl_audio_inited_) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXAPU_ERROR("RawPcmPlayer: SDL_InitSubSystem(AUDIO) failed: {}", SDL_GetError());
      return false;
    }
    sdl_audio_inited_ = true;
  }

  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_S16LE;
  spec.channels = channels ? channels : 2;
  spec.freq = sample_rate ? static_cast<int>(sample_rate) : 44100;

  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    REXAPU_ERROR("RawPcmPlayer: SDL_OpenAudioDeviceStream failed: {}", SDL_GetError());
    return false;
  }
  SDL_SetAudioStreamGain(stream_, REXCVAR_GET(audio_mute) ? 0.0f : volume_.load());
  SDL_ResumeAudioStreamDevice(stream_);

  bool ok =
      SDL_PutAudioStreamData(stream_, pcm.data(), static_cast<int>(pcm.size() * sizeof(int16_t)));
  if (!ok) {
    REXAPU_ERROR("RawPcmPlayer: SDL_PutAudioStreamData failed: {}", SDL_GetError());
  }
  // Marks the end of this one-shot clip's data so the stream drains and
  // stops requesting more, rather than looping/blocking on silence.
  SDL_FlushAudioStream(stream_);
  return ok;
}

void RawPcmPlayer::Stop() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void RawPcmPlayer::SetVolume(float volume) {
  volume_.store(volume);
  if (stream_) {
    SDL_SetAudioStreamGain(stream_, REXCVAR_GET(audio_mute) ? 0.0f : volume);
  }
}

}  // namespace rex::audio
