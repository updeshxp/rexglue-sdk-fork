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

#include <rex/kernel/xam/apps/xmp_app.h>

#include <vector>

#include <algorithm>

#include <rex/audio/wma_player.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XmpApp::XmpApp(KernelState* kernel_state)
    : App(kernel_state, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kInOrder),
      repeat_mode_(RepeatMode::kPlaylist),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1) {}

XmpApp::~XmpApp() = default;

const std::u16string XmpApp::kEmptyPath_;

void XmpApp::PlayKnownSong(size_t index) {
  if (index >= known_songs_.size()) {
    return;
  }
  const auto& song = known_songs_[index];

  std::vector<uint8_t> data;
  if (!ReadSongFile(song.file_path, data)) {
    return;
  }

  if (!wma_player_) {
    wma_player_ = std::make_unique<rex::audio::WmaPlayer>();
  }

  std::vector<std::vector<uint8_t>> buffers;
  buffers.emplace_back(std::move(data));

  // If this song has a "…2.wma" companion, append it so WmaPlayer plays the
  // intro once then loops the companion — same behaviour as the title playlist.
  wma_track_to_known_index_.clear();
  wma_track_to_known_index_.push_back(static_cast<int>(index));
  int companion_ki = FindCompanionIndex(song.file_path);
  if (companion_ki >= 0) {
    std::vector<uint8_t> companion_data;
    if (ReadSongFile(known_songs_[companion_ki].file_path, companion_data)) {
      buffers.emplace_back(std::move(companion_data));
      wma_track_to_known_index_.push_back(companion_ki);
    }
  }

  bool loop = IsInRepeatMode();
  wma_player_->Stop();
  state_ = State::kPlaying;
  playing_known_index_ = static_cast<int>(index);
  wma_player_->SetPlaybackFinishedCallback(loop ? std::function<void()>()
                                                : [this] { OnPlaybackFinishedNaturally(); });
  wma_player_->PlayPlaylist(std::move(buffers), 0, loop);
  wma_player_->SetVolume(volume_);
  wma_player_->SetSongChangedCallback([this](size_t wma_index) {
    if (wma_index < wma_track_to_known_index_.size()) {
      playing_known_index_ = wma_track_to_known_index_[wma_index];
    }
    OnStateChanged();
  });
  if (locked_) {
    // An explicit pick re-targets what the lock keeps playing.
    locked_known_index_ = static_cast<int>(index);
  }
  OnStateChanged();
}

namespace {
bool PathsEqualInsensitive(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    auto ca = a[i];
    auto cb = b[i];
    if (ca >= u'A' && ca <= u'Z')
      ca += 32;
    if (cb >= u'A' && cb <= u'Z')
      cb += 32;
    if (ca == u'/')
      ca = u'\\';
    if (cb == u'/')
      cb = u'\\';
    if (ca != cb)
      return false;
  }
  return true;
}

void CollectWmaEntries(rex::filesystem::Entry* entry, const std::string& prefix,
                       std::vector<XmpApp::Song>& out) {
  for (auto& child : entry->children()) {
    std::string child_path = prefix + child->name();
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      CollectWmaEntries(child.get(), child_path + "\\", out);
    } else {
      auto& name = child->name();
      if (name.size() >= 4) {
        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".wma") {
          XmpApp::Song song{};
          song.file_path = rex::string::to_utf16(child_path);
          song.name = rex::string::to_utf16(name.substr(0, name.size() - 4));
          out.push_back(std::move(song));
        }
      }
    }
  }
}
}  // namespace

void XmpApp::ScanFilesystem() {
  auto* fs = kernel_state_->file_system();
  if (!fs) {
    return;
  }
  const char* roots[] = {"game:\\", "update:\\"};
  for (auto* root : roots) {
    auto* entry = fs->ResolvePath(root);
    if (entry) {
      CollectWmaEntries(entry, root, known_songs_);
    }
  }
  REXKRNL_INFO("XMP: filesystem scan found {} WMA track(s)", known_songs_.size());
}

void XmpApp::SetVolume(float v) {
  volume_ = v;
  if (locked_) {
    locked_volume_ = v;
  }
  if (wma_player_) {
    wma_player_->SetVolume(v);
  }
}

void XmpApp::SetLocked(bool locked) {
  if (locked && !locked_) {
    locked_known_index_ = playing_known_index_;
    locked_volume_ = volume_;
  } else if (!locked && locked_) {
    // Replay whatever the guest most recently asked for while we were
    // pinning the audible volume, instead of waiting for its next call.
    if (wma_player_) {
      wma_player_->SetVolume(volume_);
    }
  }
  locked_ = locked;
}

bool XmpApp::ReadSongFile(const std::u16string& guest_path, std::vector<uint8_t>& out) {
  std::string path = rex::string::to_utf8(guest_path);
  if (path.empty()) {
    return false;
  }
  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action;
  X_STATUS status = kernel_state_->file_system()->OpenFile(
      nullptr, path, rex::filesystem::FileDisposition::kOpen,
      rex::filesystem::FileAccess::kFileReadData, false, true, &file, &action);
  if (XFAILED(status) || !file) {
    REXKRNL_WARN("XMP: could not open BGM file '{}' (status {:08X})", path, status);
    return false;
  }

  size_t size = file->entry() ? file->entry()->size() : 0;
  if (size == 0) {
    file->Destroy();
    REXKRNL_WARN("XMP: BGM file '{}' is empty", path);
    return false;
  }

  out.resize(size);
  size_t total = 0;
  while (total < size) {
    size_t bytes_read = 0;
    X_STATUS rs =
        file->ReadSync(std::span<uint8_t>(out.data() + total, size - total), total, &bytes_read);
    if (XFAILED(rs) || bytes_read == 0) {
      break;
    }
    total += bytes_read;
  }
  file->Destroy();

  if (total != size) {
    REXKRNL_WARN("XMP: short read on BGM file '{}' ({} of {})", path, total, size);
    out.resize(total);
  }
  return !out.empty();
}

int XmpApp::FindCompanionIndex(const std::u16string& file_path) const {
  const std::u16string kSuffix1 = u"1.wma";
  if (file_path.size() <= kSuffix1.size()) {
    return -1;
  }
  std::u16string tail = file_path.substr(file_path.size() - kSuffix1.size());
  for (auto& c : tail) {
    if (c >= u'A' && c <= u'Z')
      c += 32;
  }
  if (tail != kSuffix1) {
    return -1;
  }
  std::u16string companion_path = file_path.substr(0, file_path.size() - kSuffix1.size());
  companion_path += u"2.wma";
  for (size_t i = 0; i < known_songs_.size(); ++i) {
    if (PathsEqualInsensitive(known_songs_[i].file_path, companion_path)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void XmpApp::OnPlaybackFinishedNaturally() {
  // Called on the WmaPlayer's audio thread when a non-looping playlist
  // reaches its end on its own (as opposed to XMPStop). Mirrors what real
  // XMP does: go idle and notify, so guests polling XMPGetStatus/waiting on
  // kMsgStateChanged see the song is over instead of stalling forever.
  active_playlist_ = nullptr;
  active_song_index_ = 0;
  playing_known_index_ = -1;
  state_ = State::kIdle;
  OnStateChanged();
}

int XmpApp::CompanionIndexOf(size_t known_index) const {
  if (known_index >= known_songs_.size()) {
    return -1;
  }
  return FindCompanionIndex(known_songs_[known_index].file_path);
}

void XmpApp::StartActivePlaylist() {
  if (!active_playlist_ || active_playlist_->songs.empty()) {
    return;
  }
  if (playback_client_ == PlaybackClient::kSystem) {
    return;
  }

  // Build the track list, appending a "part 2" companion for any song whose
  // name ends in "1" (e.g. artbgm1 -> artbgm2). The companion plays after the
  // intro and then loops forever in WmaPlayer::ThreadMain.
  struct TrackEntry {
    std::u16string file_path;
    std::u16string name;
  };
  std::vector<TrackEntry> tracks;
  wma_track_to_song_index_.clear();
  wma_track_to_known_index_.clear();
  auto known_index_for = [this](const std::u16string& path) -> int {
    for (size_t i = 0; i < known_songs_.size(); ++i) {
      if (PathsEqualInsensitive(known_songs_[i].file_path, path)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  for (int si = 0; si < static_cast<int>(active_playlist_->songs.size()); ++si) {
    auto& song = active_playlist_->songs[si];
    tracks.push_back({song->file_path, song->name});
    wma_track_to_song_index_.push_back(si);
    wma_track_to_known_index_.push_back(known_index_for(song->file_path));
    // Detect intro/loop split via the shared FindCompanionIndex helper.
    int companion_ki = FindCompanionIndex(song->file_path);
    if (companion_ki >= 0) {
      const auto& companion = known_songs_[companion_ki];
      tracks.push_back({companion.file_path, companion.name});
      wma_track_to_song_index_.push_back(si);
      wma_track_to_known_index_.push_back(companion_ki);
    }
  }

  std::vector<std::vector<uint8_t>> buffers;
  buffers.reserve(tracks.size());
  for (auto& track : tracks) {
    std::vector<uint8_t> data;
    if (!ReadSongFile(track.file_path, data)) {
      buffers.clear();
      break;
    }
    buffers.emplace_back(std::move(data));
  }
  if (buffers.empty()) {
    REXKRNL_WARN("XMP: no BGM tracks could be loaded; music will be silent");
    return;
  }

  if (!wma_player_) {
    wma_player_ = std::make_unique<rex::audio::WmaPlayer>();
  }
  size_t start = active_song_index_ >= 0 ? static_cast<size_t>(active_song_index_) : 0;
  bool loop = IsInRepeatMode();
  wma_player_->SetPlaybackFinishedCallback(loop ? std::function<void()>()
                                                : [this] { OnPlaybackFinishedNaturally(); });
  wma_player_->PlayPlaylist(std::move(buffers), start, loop);
  wma_player_->SetVolume(volume_);
  wma_player_->SetSongChangedCallback([this](size_t wma_index) {
    if (wma_index < wma_track_to_song_index_.size()) {
      active_song_index_ = wma_track_to_song_index_[wma_index];
    }
    if (wma_index < wma_track_to_known_index_.size()) {
      playing_known_index_ = wma_track_to_known_index_[wma_index];
    }
    OnStateChanged();
  });
  REXKRNL_INFO("XMP: started BGM playlist '{}' ({} track(s))",
               rex::string::to_utf8(active_playlist_->name), tracks.size());
  for (size_t i = 0; i < tracks.size(); ++i) {
    const auto& fp = tracks[i].file_path;
    auto slash = fp.find_last_of(u"/\\");
    auto stem = slash == std::u16string::npos ? fp : fp.substr(slash + 1);
    auto dot = stem.rfind(u'.');
    if (dot != std::u16string::npos)
      stem = stem.substr(0, dot);
    REXKRNL_INFO("XMP: track[{}] = '{}'", i, rex::string::to_utf8(stem));
  }
}

X_HRESULT XmpApp::XMPGetStatus(uint32_t state_ptr) {
  if (!XThread::GetCurrentThread()->main_thread()) {
    // Some stupid games will hammer this on a thread - induce a delay
    // here to keep from starving real threads.
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }

  REXKRNL_TRACE("XMPGetStatus({:08X})", state_ptr);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                                   static_cast<uint32_t>(state_));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                         uint32_t playlist_name_ptr,
                                         const std::u16string& playlist_name, uint32_t flags,
                                         uint32_t out_song_handles, uint32_t out_playlist_handle) {
  REXKRNL_DEBUG(
      "XMPCreateTitlePlaylist({:08X}, {:08X}, {:08X}({}), {:08X}, {:08X}, "
      "{:08X})",
      songs_ptr, song_count, playlist_name_ptr, rex::string::to_utf8(playlist_name), flags,
      out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = playlist_name;
  playlist->flags = flags;
  if (songs_ptr) {
    for (uint32_t i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      uint8_t* song_base = memory_->TranslateVirtual(songs_ptr + (i * 36));
      song->file_path = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 0)));
      song->name = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 4)));
      song->artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 8)));
      song->album = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 12)));
      song->album_artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 16)));
      song->genre = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 20)));
      song->track_number = memory::load_and_swap<uint32_t>(song_base + 24);
      song->duration_ms = memory::load_and_swap<uint32_t>(song_base + 28);
      song->format = static_cast<Song::Format>(memory::load_and_swap<uint32_t>(song_base + 32));
      if (out_song_handles) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_song_handles + (i * 4)),
                                         song->handle);
      }
      REXKRNL_DEBUG("XMPCreateTitlePlaylist: song[{}] path='{}' name='{}'", i,
                    rex::string::to_utf8(song->file_path), rex::string::to_utf8(song->name));
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_playlist_handle),
                                     playlist->handle);
  }

  for (auto& s : playlist->songs) {
    bool found = false;
    for (auto& k : known_songs_) {
      if (PathsEqualInsensitive(k.file_path, s->file_path)) {
        found = true;
        break;
      }
    }
    if (!found) {
      known_songs_.push_back(*s);
    }
  }

  auto global_lock = global_critical_region_.Acquire();
  playlists_.insert({playlist->handle, playlist.get()});
  playlist.release();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  REXKRNL_DEBUG("XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  auto global_lock = global_critical_region_.Acquire();
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
    return X_E_NOTFOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  delete playlist;
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle) {
  REXKRNL_DEBUG("XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
      return X_E_NOTFOUND;
    }
    playlist = it->second;
  }

  if (playback_client_ == PlaybackClient::kSystem) {
    REXKRNL_WARN("XMPPlayTitlePlaylist: System playback is enabled!");
    return X_E_SUCCESS;
  }

  active_playlist_ = playlist;
  active_song_index_ = 0;
  state_ = State::kPlaying;

  if (locked_ && locked_known_index_ >= 0) {
    // Tell the guest its new playlist is playing (so any level-load logic
    // waiting on the state/notification doesn't stall) without touching the
    // audio engine -- the locked song keeps playing uninterrupted.
    playing_known_index_ = locked_known_index_;
    OnStateChanged();
    kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
    return X_E_SUCCESS;
  }

  playing_known_index_ = -1;
  if (!playlist->songs.empty()) {
    const auto& fp = playlist->songs[0]->file_path;
    for (size_t i = 0; i < known_songs_.size(); ++i) {
      if (PathsEqualInsensitive(known_songs_[i].file_path, fp)) {
        playing_known_index_ = static_cast<int>(i);
        break;
      }
    }
  }
  StartActivePlaylist();
  OnStateChanged();
  kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPContinue() {
  REXKRNL_DEBUG("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
  }
  if (wma_player_) {
    wma_player_->Resume();
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(uint32_t unk, bool user_initiated) {
  assert_zero(unk);
  REXKRNL_DEBUG("XMPStop({:08X})", unk);
  active_playlist_ = nullptr;  // ?
  active_song_index_ = 0;
  state_ = State::kIdle;

  if (locked_ && !user_initiated) {
    // Report the stop to the guest (so it doesn't stall waiting for one) but
    // leave the locked song's audio untouched.
    OnStateChanged();
    return X_E_SUCCESS;
  }

  playing_known_index_ = -1;
  if (wma_player_) {
    wma_player_->Stop();
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  REXKRNL_DEBUG("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  if (wma_player_) {
    wma_player_->Pause();
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  REXKRNL_DEBUG("XMPNext()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ = (active_song_index_ + 1) % active_playlist_->songs.size();
  StartActivePlaylist();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  REXKRNL_DEBUG("XMPPrevious()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  if (!active_song_index_) {
    active_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
  } else {
    --active_song_index_;
  }
  StartActivePlaylist();
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged, static_cast<uint32_t>(state_));
}

X_HRESULT XmpApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t storage_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t song_handle = memory::load_and_swap<uint32_t>(buffer + 8);  // 0?
      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(storage_ptr));
      assert_true(xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t unk = memory::load_and_swap<uint32_t>(buffer + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode;
        rex::be<uint32_t> repeat_mode;
        rex::be<uint32_t> flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})", uint32_t(args->playback_mode),
                    uint32_t(args->repeat_mode), uint32_t(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(uint32_t(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(uint32_t(args->repeat_mode));
      unknown_flags_ = args->flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t state_ptr = memory::load_and_swap<uint32_t>(buffer + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetVolume({:08X})", uint32_t(args->volume_ptr));
      memory::store_and_swap<float>(memory_->TranslateVirtual(args->volume_ptr), volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<float> value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPSetVolume({:g})", float(args->value));
      volume_ = args->value;
      if (locked_) {
        // Keep the guest's own bookkeeping consistent (so a later
        // XMPGetVolume reads back what it thinks it set) but don't actually
        // change the audible volume -- it stays pinned at the locked value
        // until the user unlocks, at which point SetLocked() replays this.
        return X_E_SUCCESS;
      }
      if (wma_player_) {
        wma_player_->SetVolume(volume_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
        rex::be<uint32_t> storage_size;
        rex::be<uint32_t> songs_ptr;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> playlist_name_ptr;
        rex::be<uint32_t> flags;
        rex::be<uint32_t> song_handles_ptr;
        rex::be<uint32_t> playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 36);

      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playlist_handle_ptr),
                                       args->storage_ptr);
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = memory::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // dummy_alloc_ptr is the result of a XamAlloc of storage_size.
      assert_true(uint32_t(args->storage_size) == 4 + uint32_t(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->songs_ptr, args->song_count, args->playlist_name_ptr,
                                    playlist_name, args->flags, args->song_handles_ptr,
                                    args->storage_ptr);
    }
    case 0x0007000E: {
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> unk_ptr;  // 0
        rex::be<uint32_t> info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      auto info = memory_->TranslateVirtual(args->info_ptr);
      assert_true(args->xmp_client == 0x00000002);
      assert_zero(args->unk_ptr);
      REXKRNL_ERROR("XMPGetInfo?({:08X}, {:08X})", uint32_t(args->unk_ptr),
                    uint32_t(args->info_ptr));
      if (!active_playlist_) {
        return X_E_FAIL;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      memory::store_and_swap<uint32_t>(info + 0, song->handle);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 0, song->name);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 40, song->artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 80, song->album);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 120, song->album_artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 160, song->genre);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 200, song->track_number);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 204, song->duration_ms);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 208, static_cast<uint32_t>(song->format));
      return X_E_SUCCESS;
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(args->storage_ptr));
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      // XMPSetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller;
        rex::be<uint32_t> playback_client;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true((args->xmp_client == 0x00000002 && args->controller == 0x00000000) ||
                  (args->xmp_client == 0x00000000 && args->controller == 0x00000001));
      REXKRNL_DEBUG("XMPSetPlaybackController({:08X}, {:08X})", uint32_t(args->controller),
                    uint32_t(args->playback_client));

      playback_client_ = PlaybackClient(uint32_t(args->playback_client));
      kernel_state_->BroadcastNotification(kMsgPlaybackControllerChanged, !args->playback_client);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      // XMPGetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller_ptr;
        rex::be<uint32_t> locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetPlaybackController({:08X}, {:08X}, {:08X})", uint32_t(args->xmp_client),
                    uint32_t(args->controller_ptr), uint32_t(args->locked_ptr));
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->controller_ptr), 0);
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->locked_ptr), 0);

      if (!XThread::GetCurrentThread()->main_thread()) {
        // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
        rex::thread::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      // XMPGetPlaybackBehavior
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode_ptr;
        rex::be<uint32_t> repeat_mode_ptr;
        rex::be<uint32_t> unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                    uint32_t(args->playback_mode_ptr), uint32_t(args->repeat_mode_ptr),
                    uint32_t(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playback_mode_ptr),
                                         static_cast<uint32_t>(playback_mode_));
      }
      if (args->repeat_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->repeat_mode_ptr),
                                         static_cast<uint32_t>(repeat_mode_));
      }
      if (args->unk3_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->unk3_ptr), unknown_flags_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      // We don't use the storage, so just fudge the number.
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->size_ptr),
                                       4 + uint32_t(args->song_count) * 128);
      return X_E_SUCCESS;
    }
    case 0x0007003D: {
      // XMPCaptureOutput - not sure how this works :/
      REXKRNL_DEBUG("XMPCaptureOutput(...)");
      assert_always("XMP output not unimplemented");
      return X_E_FAIL;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
