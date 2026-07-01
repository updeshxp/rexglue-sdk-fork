// rexglue - WMA/ASF streaming player
//
// Standalone background-music player for titles that drive BGM through the
// Xbox 360 XMP "title playlist" API (XMPCreateTitlePlaylist / XMPPlayTitlePlaylist)
// using bundled .wma files. The SDK ships a deliberately trimmed FFmpeg
// (libavcodec + libavutil only, no libavformat), so this parses the ASF
// container itself and feeds raw WMA packets to the libavcodec WMA decoder,
// streaming the decoded PCM to its own SDL audio device stream that is
// independent of the title's XAudio output.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct SDL_AudioStream;
struct AVCodec;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace rex::audio {

class WmaPlayer {
 public:
  WmaPlayer();
  ~WmaPlayer();

  WmaPlayer(const WmaPlayer&) = delete;
  WmaPlayer& operator=(const WmaPlayer&) = delete;

  // Begins playback of an in-memory playlist of ASF/WMA songs, starting at
  // |start_index|. When the final song finishes it is repeated (unless |loop|
  // is false, in which case playback stops and on_playback_finished_ fires),
  // reproducing the intro+loop behaviour titles get from XMP title playlists.
  // Replaces any current playback. Returns false if no song could be
  // parsed/decoded.
  bool PlayPlaylist(std::vector<std::vector<uint8_t>> songs, size_t start_index, bool loop = true);

  void Stop();
  void Pause();
  void Resume();
  void SetVolume(float volume);

  // Called on the audio thread whenever the player advances to a new song.
  void SetSongChangedCallback(std::function<void(size_t)> cb);
  // Called on the audio thread when playback reaches the end of a
  // non-looping playlist (see |loop| in PlayPlaylist) and stops on its own,
  // as opposed to being stopped explicitly via Stop(). Lets callers (e.g.
  // XmpApp) transition their own state/notifications the way a real XMP
  // "song finished naturally" would.
  void SetPlaybackFinishedCallback(std::function<void()> cb);

  bool is_playing() const { return playing_.load(); }
  size_t song_index() const { return song_index_; }

 private:
  // Per-song WAVEFORMATEX + ASF data-packet layout.
  struct Track {
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t avg_bytes = 0;
    uint16_t block_align = 0;
    uint16_t bits = 0;
    uint8_t stream_number = 0;
    std::vector<uint8_t> extradata;
    size_t packet_size = 0;       // fixed ASF data-packet size
    size_t data_packet_base = 0;  // file offset of first data packet
    size_t data_packet_count = 0;
  };

  bool ParseAsf(const std::vector<uint8_t>& data, Track& out);
  // Demuxes one ASF data packet at |packet_index|, appending any completed
  // WMA codec packets to codec_packets_.
  void DemuxPacket(const std::vector<uint8_t>& data, const Track& track, size_t packet_index);

  bool OpenDecoder(const Track& track);
  void CloseDecoder();
  void OpenSong(size_t index);
  void CloseStream();

  void ThreadMain();
  void StopLocked();
  void PushFrame();
  bool DrainDecoder();  // send remaining buffered frames to output

  std::vector<std::vector<uint8_t>> songs_;
  size_t song_index_ = 0;
  std::function<void(size_t)> on_song_changed_;
  std::function<void()> on_playback_finished_;

  // Active-song decode state.
  Track track_;
  size_t demux_index_ = 0;
  std::deque<std::vector<uint8_t>> codec_packets_;
  // Media-object reassembly across ASF payloads.
  std::vector<uint8_t> mo_buffer_;
  size_t mo_size_ = 0;
  bool mo_active_ = false;

  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  std::vector<uint8_t> pkt_buf_;  // padded scratch buffer for the current packet

  SDL_AudioStream* stream_ = nullptr;
  bool sdl_audio_inited_ = false;

  std::thread thread_;
  std::mutex mutex_;
  std::atomic<bool> running_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> paused_{false};
  std::atomic<float> volume_{1.0f};
  bool loop_ = true;
};

}  // namespace rex::audio
