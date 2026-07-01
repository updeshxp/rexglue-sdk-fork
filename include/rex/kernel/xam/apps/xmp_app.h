#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/thread/mutex.h>

namespace rex::audio {
class WmaPlayer;
}  // namespace rex::audio

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

// Only source of docs for a lot of these functions:
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Scenes/Media/Music/ScnMusic.cpp

class XmpApp : public system::xam::App {
 public:
  enum class State : uint32_t {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  enum class PlaybackClient : uint32_t {
    kSystem = 0,
    kTitle = 1,
  };
  enum class PlaybackMode : uint32_t {
    kInOrder = 0,
    kShuffle = 1,
  };
  enum class RepeatMode : uint32_t {
    kPlaylist = 0,
    kNoRepeat = 1,
  };
  struct Song {
    enum class Format : uint32_t {
      kWma = 0,
      kMp3 = 1,
    };

    uint32_t handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    uint32_t track_number;
    uint32_t duration_ms;
    Format format;
  };
  struct Playlist {
    uint32_t handle;
    std::u16string name;
    uint32_t flags;
    std::vector<std::unique_ptr<Song>> songs;
  };

  explicit XmpApp(system::KernelState* kernel_state);
  ~XmpApp();

  State state() const { return state_; }
  const Playlist* active_playlist() const { return active_playlist_; }
  int active_song_index() const { return active_song_index_; }
  const std::vector<Song>& known_songs() const { return known_songs_; }
  int playing_known_index() const { return playing_known_index_; }
  void PlayKnownSong(size_t index);
  void ScanFilesystem();
  // Raw volume as the guest last requested it -- while locked this can
  // differ from what's actually audible (see audible_volume()).
  float volume() const { return volume_; }
  // The volume actually being played right now.
  float audible_volume() const { return locked_ ? locked_volume_ : volume_; }
  void SetVolume(float v);
  // When locked, guest-driven playlist changes (e.g. the title switching BGM
  // on a room transition) leave the currently-locked song audibly playing.
  // The guest still sees normal state transitions/notifications so its own
  // logic (e.g. a level-load fade) doesn't stall waiting on them -- only the
  // actual audio output is pinned. An explicit PlayKnownSong (the overlay's
  // track list / transport buttons) always works and re-targets the lock to
  // whatever was just picked. Guest-driven volume changes (XMPSetVolume)
  // still update the guest's bookkeeping while locked (so it isn't left
  // thinking a change failed) but the audible volume stays pinned to
  // whatever it was at lock time -- SetVolume (the overlay's slider) always
  // applies immediately and re-targets the pin. Unlocking replays the
  // guest's latest requested volume so it doesn't wait for its next call.
  bool locked() const { return locked_; }
  void SetLocked(bool locked);
  // Returns the known_songs_ index of the "…2.wma" companion for
  // known_songs_[known_index] (which must end in "…1.wma"), or -1 if none.
  int CompanionIndexOf(size_t known_index) const;

  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  X_HRESULT XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr, const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle);
  X_HRESULT XMPContinue();
  // |user_initiated| is true only for the overlay's own Stop button; guest
  // (and internal) callers leave it false so a stop while locked keeps the
  // locked song audible instead of actually stopping playback.
  X_HRESULT XMPStop(uint32_t unk, bool user_initiated = false);
  X_HRESULT XMPPause();
  X_HRESULT XMPNext();
  X_HRESULT XMPPrevious();

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  static const uint32_t kMsgStateChanged = 0x0A000001;
  static const uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

  void OnStateChanged();

  // Reads a guest file (by its XMP song file path) into |out|. Returns false on
  // failure.
  bool ReadSongFile(const std::u16string& guest_path, std::vector<uint8_t>& out);
  // Decodes/streams the active playlist's songs (starting at active_song_index_)
  // through the WMA player. Real BGM output for titles that use XMP playlists.
  void StartActivePlaylist();
  // Returns the known_songs_ index of the companion "…2.wma" for a path ending
  // in "…1.wma", or -1 if there is no such companion in known_songs_.
  int FindCompanionIndex(const std::u16string& file_path) const;
  // Whether the active playlist should repeat once it reaches its last
  // track, per the guest's most recent XMPSetPlaybackBehavior call.
  bool IsInRepeatMode() const { return repeat_mode_ != RepeatMode::kNoRepeat; }
  // Wired up as the WmaPlayer's playback-finished callback whenever a
  // non-looping playlist is started; mirrors the state transition/
  // notification a real XMP client sees when a song finishes naturally.
  void OnPlaybackFinishedNaturally();

  std::unique_ptr<rex::audio::WmaPlayer> wma_player_;

  State state_;
  PlaybackClient playback_client_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  uint32_t unknown_flags_;
  float volume_;
  Playlist* active_playlist_;
  int active_song_index_;

  rex::thread::global_critical_region global_critical_region_;
  std::unordered_map<uint32_t, Playlist*> playlists_;
  uint32_t next_playlist_handle_;
  uint32_t next_song_handle_;
  std::vector<Song> known_songs_;
  int playing_known_index_ = -1;
  bool locked_ = false;
  // known_songs_ index the lock should keep playing; -1 if none captured yet.
  int locked_known_index_ = -1;
  // Audible volume the lock pins playback to while locked.
  float locked_volume_ = 1.0f;
  // Maps WmaPlayer track index -> active_playlist_ song index.
  std::vector<int> wma_track_to_song_index_;
  // Maps WmaPlayer track index -> known_songs_ index (-1 if not found).
  std::vector<int> wma_track_to_known_index_;
  static const std::u16string kEmptyPath_;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
