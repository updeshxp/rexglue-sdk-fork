// rexglue - plays a pre-baked frame-sequence clip. See baked_clip_player.h.

#include <rex/ui/baked_clip_player.h>

#include <cstdio>
#include <cstring>
#include <fstream>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/image_decode.h>

namespace rex::ui {

BakedClipPlayer::BakedClipPlayer() = default;
BakedClipPlayer::~BakedClipPlayer() = default;

size_t BakedClipPlayer::LoadFrames(ImmediateDrawer* immediate_drawer,
                                   const std::filesystem::path& frames_dir, const char* ext,
                                   size_t max_frames) {
  frames_.clear();
  if (!immediate_drawer) {
    REXLOG_INFO("BakedClipPlayer::LoadFrames: immediate_drawer is null");
    return 0;
  }

  std::string clean_ext = ext ? ext : "png";
  if (!clean_ext.empty() && clean_ext.front() == '.') {
    clean_ext.erase(clean_ext.begin());
  }

  for (size_t i = 1; max_frames == 0 || i <= max_frames; ++i) {
    char name[40];
    std::snprintf(name, sizeof(name), "frame_%04zu.%s", i, clean_ext.c_str());
    std::filesystem::path path = frames_dir / name;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      REXLOG_INFO("BakedClipPlayer::LoadFrames: could not open '{}'", path.string());
      break;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba = DecodeImageRGBA(bytes.data(), bytes.size(), width, height);
    if (rgba.empty()) {
      REXLOG_INFO("BakedClipPlayer::LoadFrames: decode failed for '{}' ({} bytes read)",
                  path.string(), bytes.size());
      break;
    }

    auto texture =
        immediate_drawer->CreateTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                        ImmediateTextureFilter::kLinear, false, rgba.data());
    if (!texture) {
      REXLOG_INFO("BakedClipPlayer::LoadFrames: CreateTexture failed for '{}' ({}x{})",
                  path.string(), width, height);
      break;
    }
    frames_.push_back(std::move(texture));
  }
  return frames_.size();
}

bool BakedClipPlayer::LoadAudio(const std::filesystem::path& wav_path) {
  pcm_.clear();
  sample_rate_ = 0;
  channels_ = 0;

  std::ifstream file(wav_path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    return false;
  }

  size_t cursor = 12;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  bool found_data = false;
  while (cursor + 8 <= bytes.size()) {
    char chunk_id[5] = {};
    std::memcpy(chunk_id, bytes.data() + cursor, 4);
    uint32_t chunk_size;
    std::memcpy(&chunk_size, bytes.data() + cursor + 4, 4);
    size_t chunk_data = cursor + 8;
    if (chunk_data + chunk_size > bytes.size()) {
      break;
    }

    if (std::memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
      std::memcpy(&channels, bytes.data() + chunk_data + 2, 2);
      std::memcpy(&sample_rate, bytes.data() + chunk_data + 4, 4);
      std::memcpy(&bits_per_sample, bytes.data() + chunk_data + 14, 2);
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      if (bits_per_sample == 16 && channels > 0) {
        size_t sample_count = chunk_size / sizeof(int16_t);
        pcm_.resize(sample_count);
        std::memcpy(pcm_.data(), bytes.data() + chunk_data, sample_count * sizeof(int16_t));
        found_data = true;
      }
      break;  // "data" is always the last chunk in a canonical WAV.
    }

    cursor = chunk_data + chunk_size + (chunk_size & 1);  // chunks are word-aligned
  }

  sample_rate_ = sample_rate;
  channels_ = channels;
  return found_data;
}

void BakedClipPlayer::Play(double fps) {
  // Audio and video are independent: a clip with frames that failed to load
  // (or one that's audio-only) should still play its sound, rather than a
  // missing video half silently taking down the audio half too.
  if (!pcm_.empty()) {
    pcm_player_.PlayInt16(pcm_, sample_rate_, channels_);
  }
  if (frames_.empty()) {
    return;
  }
  fps_ = fps > 0.0 ? fps : 30.0;
  elapsed_ = 0.0f;
  playing_ = true;
}

void BakedClipPlayer::Stop() {
  playing_ = false;
  pcm_player_.Stop();
}

void BakedClipPlayer::Update(float delta_time, const ImVec2& top_left, const ImVec2& bottom_right) {
  if (!playing_) {
    return;
  }
  elapsed_ += delta_time;
  size_t index = static_cast<size_t>(elapsed_ * fps_);
  if (index >= frames_.size()) {
    playing_ = false;
    return;
  }

  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  ImTextureID tex_id = reinterpret_cast<ImTextureID>(frames_[index].get());
  draw_list->AddImage(tex_id, top_left, bottom_right);
}

}  // namespace rex::ui
