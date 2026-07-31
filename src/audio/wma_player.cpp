// rexglue - WMA/ASF streaming player. See include/rex/audio/wma_player.h.

#include <rex/audio/wma_player.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <rex/audio/ducking.h>
#include <rex/audio/flags.h>
#include <rex/logging.h>

#include <SDL3/SDL.h>

extern "C" {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/samplefmt.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

namespace {

// ASF object GUIDs (16-byte little-endian forms, as stored in the file).
constexpr uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                     0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidFileProps[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidStreamProps[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                          0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidData[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                   0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};

inline bool GuidEq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 16) == 0;
}

inline uint16_t Le16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32);
}

// Reads an ASF variable-length field. |type|: 0=>absent(0), 1=>byte, 2=>word, 3=>dword.
inline uint32_t ReadVar(const uint8_t* p, int type, size_t& cursor) {
  switch (type) {
    case 1:
      return p[cursor++];
    case 2: {
      uint16_t v = Le16(p + cursor);
      cursor += 2;
      return v;
    }
    case 3: {
      uint32_t v = Le32(p + cursor);
      cursor += 4;
      return v;
    }
    default:
      return 0;
  }
}

}  // namespace

WmaPlayer::WmaPlayer() = default;

WmaPlayer::~WmaPlayer() {
  Stop();
  if (sdl_audio_inited_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_inited_ = false;
  }
}

bool WmaPlayer::ParseAsf(const std::vector<uint8_t>& data, Track& out) {
  const uint8_t* d = data.data();
  const size_t n = data.size();
  if (n < 30 || !GuidEq(d, kGuidHeader)) {
    REXAPU_ERROR("WmaPlayer: not an ASF file");
    return false;
  }
  const uint64_t header_size = Le64(d + 16);
  if (header_size < 30 || header_size > n) {
    REXAPU_ERROR("WmaPlayer: bad ASF header size");
    return false;
  }

  bool have_props = false;
  bool have_stream = false;
  // Walk top-level header sub-objects.
  size_t p = 30;
  while (p + 24 <= header_size) {
    const uint8_t* g = d + p;
    const uint64_t obj_size = Le64(d + p + 16);
    if (obj_size < 24 || p + obj_size > header_size)
      break;

    if (GuidEq(g, kGuidFileProps)) {
      // Minimum Data Packet Size is at body offset 16+8*6+4 = 68 (after the
      // 24-byte object header).
      size_t off = p + 24 + 68;
      if (off + 4 <= n) {
        out.packet_size = Le32(d + off);
        have_props = true;
      }
    } else if (GuidEq(g, kGuidStreamProps) && !have_stream) {
      // body: streamType(16) ecType(16) timeOffset(8) typeSpecLen(4)
      //       ecLen(4) flags(2) reserved(4) -> WAVEFORMATEX
      size_t off = p + 24 + 16 + 16 + 8 + 4 + 4;
      if (off + 6 > n)
        continue;
      uint16_t flags = Le16(d + off);
      off += 2 + 4;
      if (off + 18 > n)
        continue;
      out.stream_number = uint8_t(flags & 0x7F);
      out.format_tag = Le16(d + off + 0);
      out.channels = Le16(d + off + 2);
      out.sample_rate = Le32(d + off + 4);
      out.avg_bytes = Le32(d + off + 8);
      out.block_align = Le16(d + off + 12);
      out.bits = Le16(d + off + 14);
      uint16_t cb = Le16(d + off + 16);
      size_t extra_off = off + 18;
      if (extra_off + cb <= n) {
        out.extradata.assign(d + extra_off, d + extra_off + cb);
        have_stream = true;
      }
    }
    p += obj_size;
  }

  if (!have_props || !have_stream) {
    REXAPU_ERROR("WmaPlayer: missing ASF file/stream properties");
    return false;
  }

  // Data object immediately follows the header object.
  size_t data_off = header_size;
  if (data_off + 50 > n || !GuidEq(d + data_off, kGuidData)) {
    // Fall back to scanning for it.
    data_off = SIZE_MAX;
    for (size_t i = 30; i + 16 <= n; ++i) {
      if (GuidEq(d + i, kGuidData)) {
        data_off = i;
        break;
      }
    }
    if (data_off == SIZE_MAX || data_off + 50 > n) {
      REXAPU_ERROR("WmaPlayer: ASF data object not found");
      return false;
    }
  }
  const uint64_t data_obj_size = Le64(d + data_off + 16);
  out.data_packet_base = data_off + 50;
  uint64_t avail = (data_obj_size >= 50 && data_off + data_obj_size <= n)
                       ? data_obj_size - 50
                       : n - out.data_packet_base;
  if (out.packet_size == 0) {
    REXAPU_ERROR("WmaPlayer: zero ASF packet size");
    return false;
  }
  out.data_packet_count = static_cast<size_t>(avail / out.packet_size);
  if (out.data_packet_count == 0) {
    REXAPU_ERROR("WmaPlayer: no ASF data packets");
    return false;
  }
  return true;
}

void WmaPlayer::DemuxPacket(const std::vector<uint8_t>& data, const Track& track,
                            size_t packet_index) {
  const uint8_t* d = data.data();
  const size_t base = track.data_packet_base + packet_index * track.packet_size;
  const size_t end = base + track.packet_size;
  if (end > data.size())
    return;
  size_t o = base;

  // Error-correction data (optional).
  uint8_t ec_flags = d[o];
  if (ec_flags & 0x80) {
    o += 1 + (ec_flags & 0x0F);
  }

  if (o + 2 > end)
    return;
  uint8_t length_type_flags = d[o++];
  uint8_t property_flags = d[o++];

  const bool multiple = (length_type_flags & 0x01) != 0;
  const int seq_type = (length_type_flags >> 1) & 0x03;
  const int pad_len_type = (length_type_flags >> 3) & 0x03;
  const int pkt_len_type = (length_type_flags >> 5) & 0x03;

  const int repl_type = property_flags & 0x03;
  const int off_type = (property_flags >> 2) & 0x03;
  const int mon_type = (property_flags >> 4) & 0x03;

  size_t cur = o - base;  // cursor relative to packet base, into d+base
  auto rd = [&](int t) { return ReadVar(d + base, t, cur); };

  /* packet_length */ rd(pkt_len_type);
  /* sequence */ rd(seq_type);
  uint32_t padding_length = rd(pad_len_type);
  cur += 4;  // send time
  cur += 2;  // duration

  int payload_count = 1;
  int payload_len_type = 0;
  if (multiple) {
    uint8_t pf = d[base + cur++];
    payload_count = pf & 0x3F;
    payload_len_type = (pf >> 6) & 0x03;
  }

  const size_t payload_region_end = track.packet_size - padding_length;

  for (int i = 0; i < payload_count; ++i) {
    if (base + cur >= end)
      break;
    uint8_t stream_number = d[base + cur++] & 0x7F;
    uint32_t media_object_number = rd(mon_type);
    uint32_t offset_into_media_object = rd(off_type);
    uint32_t replicated_length = rd(repl_type);

    uint32_t media_object_size = 0;
    if (replicated_length >= 8) {
      media_object_size = Le32(d + base + cur);
    }
    cur += replicated_length;

    uint32_t payload_length;
    if (multiple) {
      payload_length = rd(payload_len_type);
    } else {
      payload_length = uint32_t(payload_region_end - cur);
    }
    (void)media_object_number;

    const uint8_t* payload = d + base + cur;
    cur += payload_length;
    if (base + cur > end)
      break;

    if (stream_number != track.stream_number)
      continue;
    if (replicated_length == 1)
      continue;  // compressed payloads: unsupported

    // Reassemble media objects by offset. New object starts at offset 0.
    if (offset_into_media_object == 0) {
      if (mo_active_ && !mo_buffer_.empty()) {
        codec_packets_.emplace_back(std::move(mo_buffer_));
      }
      mo_buffer_.clear();
      mo_size_ = media_object_size ? media_object_size : payload_length;
      mo_active_ = true;
    }
    mo_buffer_.insert(mo_buffer_.end(), payload, payload + payload_length);

    if (mo_active_ && mo_buffer_.size() >= mo_size_ && mo_size_ != 0) {
      codec_packets_.emplace_back(std::move(mo_buffer_));
      mo_buffer_.clear();
      mo_active_ = false;
    }
  }
}

bool WmaPlayer::OpenDecoder(const Track& track) {
  CloseDecoder();

  AVCodecID id = track.format_tag == 0x0162 || track.format_tag == 0x0163 ? AV_CODEC_ID_WMAPRO
                                                                          : AV_CODEC_ID_WMAV2;
  codec_ = avcodec_find_decoder(id);
  if (!codec_) {
    REXAPU_ERROR("WmaPlayer: WMA decoder not available (tag {:04X})", track.format_tag);
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec_);
  if (!ctx_)
    return false;

  ctx_->channels = track.channels;
  ctx_->sample_rate = static_cast<int>(track.sample_rate);
  ctx_->block_align = track.block_align;
  ctx_->bit_rate = static_cast<int64_t>(track.avg_bytes) * 8;
  ctx_->bits_per_coded_sample = track.bits;
  if (!track.extradata.empty()) {
    ctx_->extradata =
        static_cast<uint8_t*>(av_mallocz(track.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx_->extradata) {
      CloseDecoder();
      return false;
    }
    std::memcpy(ctx_->extradata, track.extradata.data(), track.extradata.size());
    ctx_->extradata_size = static_cast<int>(track.extradata.size());
  }

  if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
    REXAPU_ERROR("WmaPlayer: failed to open WMA decoder");
    CloseDecoder();
    return false;
  }
  if (!pkt_)
    pkt_ = av_packet_alloc();
  if (!frame_)
    frame_ = av_frame_alloc();
  return pkt_ && frame_;
}

void WmaPlayer::CloseDecoder() {
  if (ctx_)
    avcodec_free_context(&ctx_);
  codec_ = nullptr;
}

void WmaPlayer::SetSongChangedCallback(std::function<void(size_t)> cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_song_changed_ = std::move(cb);
}

void WmaPlayer::SetPlaybackFinishedCallback(std::function<void()> cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_playback_finished_ = std::move(cb);
}

void WmaPlayer::OpenSong(size_t index) {
  CloseDecoder();
  codec_packets_.clear();
  mo_buffer_.clear();
  mo_active_ = false;
  mo_size_ = 0;
  demux_index_ = 0;
  song_index_ = index;
  if (on_song_changed_) {
    on_song_changed_(index);
  }

  if (index >= songs_.size())
    return;
  if (!ParseAsf(songs_[index], track_))
    return;
  if (!OpenDecoder(track_))
    return;

  // (Re)create the SDL output stream matching this song's format.
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32LE;
  spec.channels = track_.channels ? track_.channels : 2;
  spec.freq = track_.sample_rate ? static_cast<int>(track_.sample_rate) : 44100;
  if (!stream_) {
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      REXAPU_ERROR("WmaPlayer: SDL_OpenAudioDeviceStream failed: {}", SDL_GetError());
      return;
    }
    SDL_SetAudioStreamGain(stream_, REXCVAR_GET(audio_mute)
                                        ? 0.0f
                                        : volume_.load() * GetDuckFactor() *
                                              static_cast<float>(REXCVAR_GET(audio_volume)));
    SDL_ResumeAudioStreamDevice(stream_);
  } else {
    SDL_AudioSpec current_input;
    if (!SDL_GetAudioStreamFormat(stream_, &current_input, nullptr) ||
        current_input.channels != spec.channels || current_input.freq != spec.freq) {
      SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    }
  }
}

void WmaPlayer::PushFrame() {
  if (!stream_ || frame_->nb_samples <= 0)
    return;
  const int channels = ctx_->channels > 0 ? ctx_->channels : 1;
  const int samples = frame_->nb_samples;
  const AVSampleFormat fmt = static_cast<AVSampleFormat>(frame_->format);
  const bool planar = av_sample_fmt_is_planar(fmt) != 0;

  std::vector<float> interleaved(static_cast<size_t>(samples) * channels);
  for (int c = 0; c < channels; ++c) {
    const uint8_t* src = planar ? frame_->data[c] : frame_->data[0];
    for (int s = 0; s < samples; ++s) {
      float v = 0.0f;
      switch (fmt) {
        case AV_SAMPLE_FMT_FLTP:
          v = reinterpret_cast<const float*>(src)[s];
          break;
        case AV_SAMPLE_FMT_FLT:
          v = reinterpret_cast<const float*>(src)[s * channels + c];
          break;
        case AV_SAMPLE_FMT_S16P:
          v = reinterpret_cast<const int16_t*>(src)[s] / 32768.0f;
          break;
        case AV_SAMPLE_FMT_S16:
          v = reinterpret_cast<const int16_t*>(src)[s * channels + c] / 32768.0f;
          break;
        case AV_SAMPLE_FMT_S32P:
          v = reinterpret_cast<const int32_t*>(src)[s] / 2147483648.0f;
          break;
        case AV_SAMPLE_FMT_S32:
          v = reinterpret_cast<const int32_t*>(src)[s * channels + c] / 2147483648.0f;
          break;
        default:
          break;
      }
      interleaved[static_cast<size_t>(s) * channels + c] = v;
    }
  }
  SDL_PutAudioStreamData(stream_, interleaved.data(),
                         static_cast<int>(interleaved.size() * sizeof(float)));
}

bool WmaPlayer::DrainDecoder() {
  if (!ctx_)
    return false;
  avcodec_send_packet(ctx_, nullptr);
  while (avcodec_receive_frame(ctx_, frame_) == 0) {
    PushFrame();
    av_frame_unref(frame_);
  }
  avcodec_flush_buffers(ctx_);
  return true;
}

void WmaPlayer::ThreadMain() {
  using namespace std::chrono_literals;
  // Keep roughly this many bytes of decoded audio buffered ahead.
  const int target_queued = 44100 * 2 * static_cast<int>(sizeof(float)) / 2;  // ~0.5s
  float last_applied_gain = -1.0f;  // unreachable by the formula below, forces the first apply

  while (running_.load()) {
    float gain = REXCVAR_GET(audio_mute) ? 0.0f
                                         : volume_.load() * GetDuckFactor() *
                                               static_cast<float>(REXCVAR_GET(audio_volume));
    if (stream_ && gain != last_applied_gain) {
      SDL_SetAudioStreamGain(stream_, gain);
      last_applied_gain = gain;
    }

    if (paused_.load()) {
      std::this_thread::sleep_for(10ms);
      continue;
    }
    if (!ctx_ || !stream_) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    if (SDL_GetAudioStreamQueued(stream_) > target_queued) {
      std::this_thread::sleep_for(5ms);
      continue;
    }

    // Refill the codec-packet queue from the ASF stream.
    while (codec_packets_.empty() && demux_index_ < track_.data_packet_count) {
      DemuxPacket(songs_[song_index_], track_, demux_index_++);
    }

    if (codec_packets_.empty()) {
      // End of song: drain decoder, then advance (looping the final song
      // unless the playlist was started with loop = false).
      DrainDecoder();
      if (song_index_ + 1 < songs_.size()) {
        OpenSong(song_index_ + 1);
      } else if (loop_) {
        OpenSong(song_index_);
      } else {
        playing_.store(false);
        if (on_playback_finished_) {
          on_playback_finished_();
        }
        std::this_thread::sleep_for(10ms);
        continue;
      }
      if (!ctx_) {
        // Could not continue; stop playback.
        playing_.store(false);
        std::this_thread::sleep_for(10ms);
      }
      continue;
    }

    std::vector<uint8_t> cp = std::move(codec_packets_.front());
    codec_packets_.pop_front();

    // libavcodec reads past the end of packets; provide padding.
    pkt_buf_.assign(cp.begin(), cp.end());
    pkt_buf_.resize(cp.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    pkt_->data = pkt_buf_.data();
    pkt_->size = static_cast<int>(cp.size());

    int ret = avcodec_send_packet(ctx_, pkt_);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, sizeof(errbuf));
      REXAPU_DEBUG("WmaPlayer: send_packet error: {}", errbuf);
      continue;
    }
    while (avcodec_receive_frame(ctx_, frame_) == 0) {
      PushFrame();
      av_frame_unref(frame_);
    }
  }
}

bool WmaPlayer::PlayPlaylist(std::vector<std::vector<uint8_t>> songs, size_t start_index,
                             bool loop) {
  std::lock_guard<std::mutex> lock(mutex_);
  StopLocked();

  if (songs.empty())
    return false;

  loop_ = loop;

  if (!sdl_audio_inited_) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXAPU_ERROR("WmaPlayer: SDL_InitSubSystem(AUDIO) failed: {}", SDL_GetError());
      return false;
    }
    sdl_audio_inited_ = true;
  }

  songs_ = std::move(songs);
  if (start_index >= songs_.size())
    start_index = 0;

  OpenSong(start_index);
  if (!ctx_ || !stream_) {
    REXAPU_ERROR("WmaPlayer: failed to start playlist");
    return false;
  }

  running_.store(true);
  paused_.store(false);
  playing_.store(true);
  thread_ = std::thread(&WmaPlayer::ThreadMain, this);
  return true;
}

void WmaPlayer::StopLocked() {
  if (running_.load()) {
    running_.store(false);
    if (thread_.joinable())
      thread_.join();
  } else if (thread_.joinable()) {
    thread_.join();
  }
  playing_.store(false);
  paused_.store(false);
  CloseDecoder();
  if (frame_)
    av_frame_free(&frame_);
  if (pkt_)
    av_packet_free(&pkt_);
  codec_packets_.clear();
  mo_buffer_.clear();
  mo_active_ = false;
  songs_.clear();
  CloseStream();
}

void WmaPlayer::CloseStream() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void WmaPlayer::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  StopLocked();
}

void WmaPlayer::Pause() {
  paused_.store(true);
  if (stream_)
    SDL_PauseAudioStreamDevice(stream_);
}

void WmaPlayer::Resume() {
  paused_.store(false);
  if (stream_)
    SDL_ResumeAudioStreamDevice(stream_);
}

void WmaPlayer::SetVolume(float volume) {
  volume_.store(volume);
  if (stream_ && !REXCVAR_GET(audio_mute))
    SDL_SetAudioStreamGain(stream_, volume * GetDuckFactor());
}

}  // namespace rex::audio
