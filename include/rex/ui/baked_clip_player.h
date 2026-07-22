/**
 * @file        ui/baked_clip_player.h
 * @brief       Plays a pre-baked frame-sequence "clip" (images + PCM audio)
 *              fullscreen or in a rect
 *
 * @remarks     There's no video codec in this SDK (WmaPlayer's own comment
 *              notes the bundled FFmpeg is libavcodec+libavutil only, no
 *              libavformat, and even that's audio-only) -- so a mod that
 *              wants to show a short clip has to pre-bake it offline (e.g.
 *              ffmpeg extracting a numbered image sequence plus a WAV
 *              audio track) and play that back at runtime. BakedClipPlayer
 *              is exactly that playback half, factored out of the
 *              boilerplate a mod would otherwise duplicate: loading each
 *              frame via DecodeImageRGBA + ImmediateDrawer::CreateTexture,
 *              parsing a canonical PCM WAV, and driving per-frame timing
 *              against wall-clock delta time.
 *
 *              Textures can only be created once an ImmediateDrawer is
 *              attached (see ImGuiDrawer::OnImmediateDrawerReady), so
 *              LoadFrames takes one as a parameter rather than this class
 *              reaching for one itself -- call it from that callback.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/audio/raw_pcm_player.h>
#include <rex/ui/immediate_drawer.h>

struct ImVec2;

namespace rex::ui {

class BakedClipPlayer {
 public:
  BakedClipPlayer();
  ~BakedClipPlayer();

  BakedClipPlayer(const BakedClipPlayer&) = delete;
  BakedClipPlayer& operator=(const BakedClipPlayer&) = delete;

  // Loads frames named "frame_0001.<ext>", "frame_0002.<ext>", ... (1-based,
  // 4-digit, zero-padded) from |frames_dir|, stopping at the first missing
  // index or, if positive, after |max_frames|. |ext| is tried as given (a
  // leading '.' is optional, e.g. "png" or ".png"). Any stb_image-supported
  // format works (see DecodeImageRGBA). Returns the number of frames
  // loaded. Must be called with a non-null |immediate_drawer| -- see the
  // class remarks above for why that can't happen at arbitrary times.
  size_t LoadFrames(ImmediateDrawer* immediate_drawer, const std::filesystem::path& frames_dir,
                    const char* ext = "png", size_t max_frames = 0);

  // Parses a canonical PCM WAV file (the output of e.g.
  // `ffmpeg -acodec pcm_s16le`) to play alongside the frames. Optional --
  // a clip with no audio just doesn't call this. Returns false if the file
  // couldn't be read or isn't a 16-bit PCM WAV.
  bool LoadAudio(const std::filesystem::path& wav_path);

  // Starts playback from frame 0 at |fps|, playing the loaded audio (if
  // any) from its start. No-op if no frames are loaded.
  void Play(double fps);

  void Stop();
  bool is_playing() const { return playing_; }

  // Call once per real frame regardless of play state. Advances playback by
  // |delta_time| seconds and, if playing, draws the current frame on
  // ImGui's foreground draw list spanning [top_left, bottom_right).
  // Automatically stops after the last frame.
  void Update(float delta_time, const ImVec2& top_left, const ImVec2& bottom_right);

 private:
  std::vector<std::unique_ptr<ImmediateTexture>> frames_;

  std::vector<int16_t> pcm_;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  rex::audio::RawPcmPlayer pcm_player_;

  double fps_ = 30.0;
  float elapsed_ = 0.0f;
  bool playing_ = false;
};

}  // namespace rex::ui
