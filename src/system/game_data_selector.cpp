/**
 * @file        system/game_data_selector.cpp
 * @brief       GameDataSelector implementation — pre-presentation SDL native dialogs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/game_data_selector.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <span>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/crypto/sha256.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace rex::system {

// =============================================================================
// XDVDFS extraction helpers (ported from scripts/extract_game.py)
// =============================================================================
namespace {

bool HexEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// =============================================================================
// Bounds-checked file access
// =============================================================================

/// Random access over a file that is fully untrusted: every offset and length
/// in an ISO or STFS package comes from header fields the user's file gets to
/// choose. Each accessor validates the requested range against the real file
/// size, so a corrupt or hostile field can only produce a failed read, never
/// an out-of-bounds access. Nothing is ever loaded wholesale, so a 16 GiB
/// image costs 16 GiB of I/O rather than 16 GiB of RAM.
class FileReader {
 public:
  explicit FileReader(const std::filesystem::path& path)
      : ifs_(path, std::ios::binary | std::ios::ate) {
    if (ifs_) {
      auto end = ifs_.tellg();
      if (end > 0) {
        size_ = static_cast<uint64_t>(end);
      }
    }
  }

  bool ok() const { return size_ > 0; }
  uint64_t size() const { return size_; }

  /// Read exactly `len` bytes at `off`. Returns false if the range is not
  /// wholly inside the file or the read fails.
  bool Read(uint64_t off, void* dst, uint64_t len) {
    if (len == 0) {
      return true;
    }
    if (off > size_ || len > size_ - off) {
      return false;
    }
    ifs_.clear();
    ifs_.seekg(static_cast<std::streamoff>(off));
    if (!ifs_) {
      return false;
    }
    return static_cast<bool>(ifs_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len)));
  }

  std::optional<uint8_t> U8(uint64_t off) {
    uint8_t v;
    if (!Read(off, &v, 1))
      return std::nullopt;
    return v;
  }

  std::optional<uint16_t> U16BE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
  }

  std::optional<uint16_t> U16LE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  }

  std::optional<uint32_t> U24LE(uint64_t off) {
    uint8_t b[3];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16);
  }

  std::optional<uint32_t> U32BE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | b[3];
  }

  std::optional<uint32_t> U32LE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  }

  bool MagicAt(uint64_t off, std::string_view magic) {
    std::string buf(magic.size(), '\0');
    if (!Read(off, buf.data(), magic.size()))
      return false;
    return buf == magic;
  }

  /// Copy `len` bytes at `off` to `out` in chunks.
  bool CopyTo(std::ostream& out, uint64_t off, uint64_t len) {
    std::vector<char> chunk(64 * 1024);
    while (len > 0) {
      uint64_t n = std::min<uint64_t>(len, chunk.size());
      if (!Read(off, chunk.data(), n))
        return false;
      out.write(chunk.data(), static_cast<std::streamsize>(n));
      if (!out)
        return false;
      off += n;
      len -= n;
    }
    return true;
  }

 private:
  std::ifstream ifs_;
  uint64_t size_ = 0;
};

// =============================================================================
// Destination path safety
// =============================================================================

/// Join `base` with archive-supplied path components, refusing anything that
/// could escape the destination directory. Every name here is attacker
/// controlled: `..`, an absolute path, a drive letter, or an embedded
/// separator must never turn into a write outside `base`.
std::optional<std::filesystem::path> SafeJoin(const std::filesystem::path& base,
                                              std::span<const std::string> parts) {
  if (parts.empty()) {
    return std::nullopt;
  }
  std::filesystem::path rel;
  for (const std::string& part : parts) {
    if (part.empty() || part == "." || part == "..") {
      return std::nullopt;
    }
    if (part.find_first_of("/\\:") != std::string::npos) {
      return std::nullopt;
    }
    std::filesystem::path p(part);
    if (p.has_root_name() || p.has_root_directory()) {
      return std::nullopt;
    }
    // A single component must stay a single component.
    if (std::distance(p.begin(), p.end()) != 1) {
      return std::nullopt;
    }
    rel /= p;
  }

  auto root = base.lexically_normal();
  auto joined = (root / rel).lexically_normal();
  auto relative = joined.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    return std::nullopt;
  }
  return joined;
}

/// Result of an extraction: a count the caller can trust, plus whether any
/// entry failed. A partial extraction is a failure — the caller must not
/// report success for an install that is missing files.
struct ExtractResult {
  uint32_t files = 0;
  bool complete = true;
};

// =============================================================================
// XDVDFS (disc image) extraction — ported from scripts/extract_game.py
// =============================================================================

/// Game partition offsets seen in the wild; the volume descriptor sits 32
/// sectors into the partition.
constexpr uint64_t kXdvdfsPartitionOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                                0x0FD90000};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr uint64_t kXdvdfsSectorSize = 2048;

struct XdvdfsInfo {
  uint64_t game_offset;
  uint64_t root_offset;
  uint32_t root_size;
};

std::optional<XdvdfsInfo> FindXdvdfs(FileReader& reader) {
  for (uint64_t game_offset : kXdvdfsPartitionOffsets) {
    uint64_t fs_off = game_offset + 32 * kXdvdfsSectorSize;
    if (!reader.MagicAt(fs_off, kXdvdfsMagic)) {
      continue;
    }
    auto root_sector = reader.U32LE(fs_off + 20);
    auto root_size = reader.U32LE(fs_off + 24);
    if (!root_sector || !root_size) {
      continue;
    }
    if (*root_size < 13 || *root_size > 32 * 1024 * 1024) {
      continue;
    }
    return XdvdfsInfo{game_offset,
                      game_offset + static_cast<uint64_t>(*root_sector) * kXdvdfsSectorSize,
                      *root_size};
  }
  return std::nullopt;
}

constexpr int kMaxDirectoryDepth = 64;

void ExtractXdvdfsDirectory(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                            const std::filesystem::path& out_dir, int depth, ExtractResult& result);

/// Walk one directory's entry tree. The tree is a binary tree of ordinals
/// inside a single buffer; a malformed image can make it cyclic, so visited
/// ordinals are tracked instead of trusting it to terminate.
void ExtractXdvdfsEntries(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                          const std::filesystem::path& out_dir, int depth, ExtractResult& result) {
  std::vector<uint32_t> pending = {0};
  std::unordered_set<uint32_t> visited;

  while (!pending.empty()) {
    uint32_t ordinal = pending.back();
    pending.pop_back();
    if (!visited.insert(ordinal).second) {
      REXLOG_WARN("XDVDFS: cyclic directory entry (ordinal {}), skipping", ordinal);
      result.complete = false;
      continue;
    }

    uint64_t p = buffer_offset + static_cast<uint64_t>(ordinal) * 4;
    auto node_l = reader.U16LE(p);
    auto node_r = reader.U16LE(p + 2);
    auto sector = reader.U32LE(p + 4);
    auto length = reader.U32LE(p + 8);
    auto attributes = reader.U8(p + 12);
    auto name_length = reader.U8(p + 13);
    if (!node_l || !node_r || !sector || !length || !attributes || !name_length) {
      REXLOG_WARN("XDVDFS: truncated directory entry at 0x{:X}", p);
      result.complete = false;
      continue;
    }

    std::string name(*name_length, '\0');
    if (*name_length == 0 || !reader.Read(p + 14, name.data(), *name_length)) {
      REXLOG_WARN("XDVDFS: unreadable entry name at 0x{:X}", p);
      result.complete = false;
      continue;
    }

    if (*node_l) {
      pending.push_back(*node_l);
    }
    if (*node_r) {
      pending.push_back(*node_r);
    }

    const std::string name_parts[] = {name};
    auto dest = SafeJoin(out_dir, name_parts);
    if (!dest) {
      REXLOG_WARN("XDVDFS: rejected unsafe entry name '{}'", name);
      result.complete = false;
      continue;
    }

    uint64_t entry_offset = game_offset + static_cast<uint64_t>(*sector) * kXdvdfsSectorSize;

    if (*attributes & 0x10) {  // directory
      std::filesystem::create_directories(*dest);
      if (*length) {
        ExtractXdvdfsDirectory(reader, game_offset, entry_offset, *dest, depth + 1, result);
      }
      continue;
    }

    std::ofstream ofs(*dest, std::ios::binary);
    if (!ofs) {
      REXLOG_WARN("XDVDFS: cannot write {}", dest->string());
      result.complete = false;
      continue;
    }
    if (!reader.CopyTo(ofs, entry_offset, *length)) {
      REXLOG_WARN("XDVDFS: truncated file data for '{}' ({} bytes at 0x{:X})", name, *length,
                  entry_offset);
      result.complete = false;
      continue;
    }
    ++result.files;
  }
}

void ExtractXdvdfsDirectory(FileReader& reader, uint64_t game_offset, uint64_t buffer_offset,
                            const std::filesystem::path& out_dir, int depth,
                            ExtractResult& result) {
  if (depth > kMaxDirectoryDepth) {
    REXLOG_WARN("XDVDFS: directory nesting deeper than {}, giving up", kMaxDirectoryDepth);
    result.complete = false;
    return;
  }
  ExtractXdvdfsEntries(reader, game_offset, buffer_offset, out_dir, depth, result);
}

/// Extract an XDVDFS disc image. Returns the number of files written, or 0 if
/// anything at all failed.
uint32_t ExtractIsoTo(const std::filesystem::path& iso_path, const std::filesystem::path& out_dir) {
  FileReader reader(iso_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open ISO: {}", iso_path.string());
    return 0;
  }

  auto info = FindXdvdfs(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid XDVDFS image: {}", iso_path.string());
    return 0;
  }

  std::filesystem::create_directories(out_dir);
  ExtractResult result;
  ExtractXdvdfsDirectory(reader, info->game_offset, info->root_offset, out_dir, 0, result);

  if (!result.complete) {
    REXLOG_ERROR("ISO extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  return result.files;
}

// =============================================================================
// STFS (LIVE/CON/PIRS package) — one implementation, used for both XBLA
// packages and title updates. Mirrors scripts/extract_tu.py, which in turn
// mirrors the SDK's StfsContainerDevice.
// =============================================================================

constexpr uint64_t kStfsBlockSize = 0x1000;
constexpr uint32_t kStfsEndOfChain = 0xFFFFFF;
constexpr int kStfsHashLevel0 = 170;
constexpr int kStfsHashLevel1 = 28900;  // 170^2

// Absolute offsets inside the StfsHeader (XContentHeader + XContentMetadata).
constexpr uint64_t kStfsOffHeaderSize = 0x340;        // be u32
constexpr uint64_t kStfsOffVolumeDescriptor = 0x379;  // StfsVolumeDescriptor
constexpr uint64_t kStfsOffVolumeType = 0x3A9;        // be u32, 0 = STFS, 1 = SVOD

struct StfsEntry {
  std::string name;
  bool is_directory = false;
  uint32_t start_block = 0;
  uint32_t parent = 0xFFFF;
  uint64_t size = 0;
};

struct StfsInfo {
  bool read_only_format = false;
  bool root_active_index = false;
  int blocks_per_hash_table = 1;
  int block_step[2] = {0, 0};
  uint32_t file_table_block_count = 0;
  uint32_t file_table_block_number = 0;
  uint32_t total_block_count = 0;
  uint64_t data_base = 0;
};

bool IsStfsMagic(FileReader& reader) {
  return reader.MagicAt(0, "LIVE") || reader.MagicAt(0, "CON ") || reader.MagicAt(0, "PIRS");
}

std::optional<StfsInfo> ParseStfsInfo(FileReader& reader) {
  if (!IsStfsMagic(reader)) {
    return std::nullopt;
  }

  auto header_size = reader.U32BE(kStfsOffHeaderSize);
  auto volume_type = reader.U32BE(kStfsOffVolumeType);
  const uint64_t vd = kStfsOffVolumeDescriptor;
  auto flags = reader.U8(vd + 2);
  auto file_table_block_count = reader.U16LE(vd + 3);
  auto file_table_block_number = reader.U24LE(vd + 5);
  auto total_block_count = reader.U32BE(vd + 0x1C);

  if (!header_size || !volume_type || !flags || !file_table_block_count ||
      !file_table_block_number || !total_block_count) {
    REXLOG_ERROR("STFS: header is truncated");
    return std::nullopt;
  }
  if (*volume_type != 0) {
    REXLOG_ERROR("STFS: SVOD packages are not supported");
    return std::nullopt;
  }

  StfsInfo info;
  info.read_only_format = (*flags & 0x1) != 0;
  info.root_active_index = ((*flags >> 1) & 0x1) != 0;
  info.file_table_block_count = *file_table_block_count;
  info.file_table_block_number = *file_table_block_number;
  info.total_block_count = *total_block_count;
  info.blocks_per_hash_table = info.read_only_format ? 1 : 2;
  info.block_step[0] = kStfsHashLevel0 + info.blocks_per_hash_table;
  info.block_step[1] = kStfsHashLevel1 + (kStfsHashLevel0 + 1) * info.blocks_per_hash_table;
  info.data_base = ((static_cast<uint64_t>(*header_size) + kStfsBlockSize - 1) / kStfsBlockSize) *
                   kStfsBlockSize;
  return info;
}

uint64_t StfsBlockToOffset(uint32_t block_index, const StfsInfo& info) {
  uint64_t block = block_index;
  int64_t base = kStfsHashLevel0;
  for (int i = 0; i < 3; ++i) {
    block += ((block_index + base) / base) * info.blocks_per_hash_table;
    if (static_cast<int64_t>(block_index) < base) {
      break;
    }
    base *= kStfsHashLevel0;
  }
  return info.data_base + (block << 12);
}

uint64_t StfsHashOffset(uint32_t block_index, int level, const StfsInfo& info) {
  int64_t block;
  if (level == 0) {
    if (static_cast<int64_t>(block_index) < kStfsHashLevel0) {
      block = 0;
    } else {
      block = static_cast<int64_t>(block_index / kStfsHashLevel0) * info.block_step[0];
      block +=
          (static_cast<int64_t>(block_index / kStfsHashLevel1) + 1) * info.blocks_per_hash_table;
      if (static_cast<int64_t>(block_index) >= kStfsHashLevel1) {
        block += info.blocks_per_hash_table;
      }
    }
  } else if (level == 1) {
    if (static_cast<int64_t>(block_index) < kStfsHashLevel1) {
      block = info.block_step[0];
    } else {
      block = static_cast<int64_t>(block_index / kStfsHashLevel1) * info.block_step[1] +
              info.blocks_per_hash_table;
    }
  } else {
    block = info.block_step[1];
  }
  return info.data_base + (static_cast<uint64_t>(block) << 12);
}

/// Follow the level-0 hash table to the next block of a chain. The hash
/// record's info word is *big-endian*, and which copy of a hash table is live
/// is resolved top-down through the levels that actually exist for this
/// package's block count.
std::optional<uint32_t> StfsNextBlock(FileReader& reader, uint32_t block_index,
                                      const StfsInfo& info) {
  uint64_t secondary = info.root_active_index ? kStfsBlockSize : 0;
  uint64_t off0 = StfsHashOffset(block_index, 0, info);

  if (info.read_only_format) {
    secondary = 0;
  } else {
    if (info.total_block_count > static_cast<uint32_t>(kStfsHashLevel0)) {
      uint64_t off1 = StfsHashOffset(block_index, 1, info);
      if (info.total_block_count > static_cast<uint32_t>(kStfsHashLevel1)) {
        uint64_t off2 = StfsHashOffset(block_index, 2, info);
        uint32_t rec2 = (block_index / kStfsHashLevel1) % kStfsHashLevel0;
        auto word = reader.U32BE(off2 + secondary + rec2 * 0x18 + 0x14);
        if (!word)
          return std::nullopt;
        secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
      }
      uint32_t rec1 = (block_index / kStfsHashLevel0) % kStfsHashLevel0;
      auto word = reader.U32BE(off1 + secondary + rec1 * 0x18 + 0x14);
      if (!word)
        return std::nullopt;
      secondary = (*word & 0x40000000) ? kStfsBlockSize : 0;
    }
  }

  uint32_t rec0 = block_index % kStfsHashLevel0;
  auto word = reader.U32BE(off0 + secondary + rec0 * 0x18 + 0x14);
  if (!word)
    return std::nullopt;
  return *word & 0xFFFFFF;
}

std::vector<StfsEntry> ParseStfsFileTable(FileReader& reader, const StfsInfo& info) {
  std::vector<StfsEntry> entries;
  uint32_t table_block = info.file_table_block_number;

  for (uint32_t bi = 0; bi < info.file_table_block_count; ++bi) {
    if (table_block == kStfsEndOfChain) {
      break;
    }
    uint64_t block_off = StfsBlockToOffset(table_block, info);

    for (uint64_t m = 0; m < kStfsBlockSize / 0x40; ++m) {
      uint64_t off = block_off + m * 0x40;

      auto first = reader.U8(off);
      if (!first || *first == 0) {
        break;
      }
      auto name_flags = reader.U8(off + 0x28);
      if (!name_flags) {
        break;
      }
      uint8_t name_len = *name_flags & 0x3F;
      if (name_len == 0) {
        break;
      }

      std::string name(name_len, '\0');
      auto start_block = reader.U24LE(off + 0x2F);
      auto parent = reader.U16BE(off + 0x32);
      auto size = reader.U32BE(off + 0x34);
      if (!reader.Read(off, name.data(), name_len) || !start_block || !parent || !size) {
        break;
      }

      StfsEntry entry;
      entry.name = std::move(name);
      entry.is_directory = (*name_flags & 0x80) != 0;
      entry.start_block = *start_block;
      entry.parent = *parent;
      entry.size = *size;
      entries.push_back(std::move(entry));
    }

    auto next = StfsNextBlock(reader, table_block, info);
    if (!next) {
      REXLOG_WARN("STFS: file table chain broke after block {}", table_block);
      break;
    }
    table_block = *next;
  }
  return entries;
}

/// Stream one STFS file's block chain to `out`.
bool StfsCopyFile(FileReader& reader, const StfsEntry& entry, const StfsInfo& info,
                  std::ostream& out) {
  uint32_t block_index = entry.start_block;
  uint64_t remaining = entry.size;
  uint64_t guard = 0;
  const uint64_t max_blocks = static_cast<uint64_t>(info.total_block_count) + 1;

  while (remaining > 0 && block_index != kStfsEndOfChain) {
    if (++guard > max_blocks) {
      REXLOG_WARN("STFS: block chain for '{}' is cyclic", entry.name);
      return false;
    }
    uint64_t n = std::min<uint64_t>(remaining, kStfsBlockSize);
    if (!reader.CopyTo(out, StfsBlockToOffset(block_index, info), n)) {
      return false;
    }
    remaining -= n;
    if (remaining > 0) {
      auto next = StfsNextBlock(reader, block_index, info);
      if (!next) {
        return false;
      }
      block_index = *next;
    }
  }

  if (remaining > 0) {
    REXLOG_WARN("STFS: chain for '{}' ended {} bytes short", entry.name, remaining);
    return false;
  }
  return true;
}

/// Read a whole STFS file into memory. Only used for files small enough to
/// inspect (the XEX delta patch), never for bulk extraction.
bool StfsReadFile(FileReader& reader, const StfsEntry& entry, const StfsInfo& info,
                  std::string& out) {
  constexpr uint64_t kMaxInMemory = 256ull * 1024 * 1024;
  if (entry.size > kMaxInMemory) {
    REXLOG_WARN("STFS: '{}' is too large to inspect ({} bytes)", entry.name, entry.size);
    return false;
  }
  std::ostringstream oss(std::ios::binary);
  if (!StfsCopyFile(reader, entry, info, oss)) {
    return false;
  }
  out = oss.str();
  return true;
}

/// Resolve an entry's full path components by walking its parent chain.
std::optional<std::vector<std::string>> StfsEntryPath(const std::vector<StfsEntry>& entries,
                                                      size_t index) {
  std::vector<std::string> parts = {entries[index].name};
  uint32_t p = entries[index].parent;
  int depth = 0;
  while (p != 0xFFFF) {
    if (p >= entries.size() || ++depth > 100) {
      return std::nullopt;
    }
    parts.push_back(entries[p].name);
    p = entries[p].parent;
  }
  std::reverse(parts.begin(), parts.end());
  return parts;
}

/// Extract every file in an STFS package, preserving directory layout.
/// `skip` lets a caller exclude an entry it has already written itself.
ExtractResult ExtractStfsTree(FileReader& reader, const std::vector<StfsEntry>& entries,
                              const StfsInfo& info, const std::filesystem::path& out_dir,
                              const StfsEntry* skip) {
  ExtractResult result;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if (entry.is_directory || &entry == skip) {
      continue;
    }

    auto parts = StfsEntryPath(entries, i);
    if (!parts) {
      REXLOG_WARN("STFS: invalid parent chain for entry '{}'", entry.name);
      result.complete = false;
      continue;
    }
    auto dest = SafeJoin(out_dir, *parts);
    if (!dest) {
      REXLOG_WARN("STFS: rejected unsafe entry path for '{}'", entry.name);
      result.complete = false;
      continue;
    }

    std::filesystem::create_directories(dest->parent_path());
    std::ofstream ofs(*dest, std::ios::binary);
    if (!ofs) {
      REXLOG_WARN("STFS: cannot write {}", dest->string());
      result.complete = false;
      continue;
    }
    if (!StfsCopyFile(reader, entry, info, ofs)) {
      REXLOG_WARN("STFS: failed to read '{}'", entry.name);
      result.complete = false;
      continue;
    }
    ++result.files;
  }
  return result;
}

/// Extract an XBLA package. Uses the same STFS implementation as the title
/// update path, so CON/PIRS and writable (2-block hash table) packages work
/// here too.
uint32_t ExtractXblaTo(const std::filesystem::path& xbla_path,
                       const std::filesystem::path& out_dir) {
  FileReader reader(xbla_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open XBLA: {}", xbla_path.string());
    return 0;
  }

  auto info = ParseStfsInfo(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid LIVE/CON/PIRS package: {}", xbla_path.string());
    return 0;
  }

  auto entries = ParseStfsFileTable(reader, *info);
  if (entries.empty()) {
    REXLOG_ERROR("No STFS entries found in XBLA: {}", xbla_path.string());
    return 0;
  }
  REXLOG_INFO("Extracting {} STFS entries from {}", entries.size(), xbla_path.string());

  std::filesystem::create_directories(out_dir);
  auto result = ExtractStfsTree(reader, entries, *info, out_dir, nullptr);

  if (!result.complete) {
    REXLOG_ERROR("XBLA extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  if (!std::filesystem::is_regular_file(out_dir / "default.xex")) {
    REXLOG_ERROR("XBLA extraction did not produce a root default.xex");
    return 0;
  }
  return result.files;
}

// =============================================================================
// Title update extraction
// =============================================================================

/// Find the XEX delta patch inside a title update. The patch is *not*
/// necessarily the largest file in the package — updates routinely ship
/// replacement media that is bigger — so prefer entries named `*.xexp` and
/// fall back to trying files largest-first, accepting the first one that
/// actually begins with the XEX2 magic.
std::optional<size_t> FindXexpEntry(FileReader& reader, const std::vector<StfsEntry>& entries,
                                    const StfsInfo& info, std::string& xexp_data) {
  std::vector<size_t> candidates;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].is_directory || entries[i].size < 4) {
      continue;
    }
    std::string lower = entries[i].name;
    for (auto& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".xexp") == 0) {
      candidates.push_back(i);
    }
  }

  if (candidates.empty()) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!entries[i].is_directory && entries[i].size >= 4) {
        candidates.push_back(i);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](size_t a, size_t b) { return entries[a].size > entries[b].size; });
  }

  for (size_t i : candidates) {
    std::string data;
    if (!StfsReadFile(reader, entries[i], info, data)) {
      continue;
    }
    if (data.size() >= 4 && data.compare(0, 4, "XEX2") == 0) {
      xexp_data = std::move(data);
      return i;
    }
    REXLOG_DEBUG("TU: '{}' is not an XEX2 delta patch, trying next", entries[i].name);
  }
  return std::nullopt;
}

/// Extract a title update. The package is extracted whole into `update_dir`,
/// layout preserved, to be mounted as the `update:` device; the XEX delta
/// patch is then *copied* to `<xexp_dir>/default.xexp`, next to the base
/// default.xex it patches. It stays in the update tree as well, so that tree
/// remains a faithful copy of the package.
///
/// The two trees must not be merged: a title update commonly replaces data
/// files that exist in the base game under the same relative paths, so
/// extracting it over the game root would silently overwrite base game files
/// in a directory the user may have extracted themselves.
///
/// Returns the number of files written, or 0 on failure.
uint32_t ExtractTitleUpdateTo(const std::filesystem::path& tu_path,
                              const std::filesystem::path& xexp_dir,
                              const std::filesystem::path& update_dir) {
  FileReader reader(tu_path);
  if (!reader.ok()) {
    REXLOG_ERROR("Failed to open TU: {}", tu_path.string());
    return 0;
  }

  auto info = ParseStfsInfo(reader);
  if (!info) {
    REXLOG_ERROR("Not a valid LIVE/CON/PIRS package: {}", tu_path.string());
    return 0;
  }

  auto entries = ParseStfsFileTable(reader, *info);
  if (entries.empty()) {
    REXLOG_ERROR("No entries found in TU: {}", tu_path.string());
    return 0;
  }
  REXLOG_INFO("TU has {} entries", entries.size());

  std::string xexp_data;
  auto xexp_index = FindXexpEntry(reader, entries, *info, xexp_data);
  if (!xexp_index) {
    REXLOG_ERROR("No XEX delta patch (XEX2) found inside {}", tu_path.string());
    return 0;
  }
  REXLOG_INFO("TU delta patch: '{}' ({} bytes)", entries[*xexp_index].name, xexp_data.size());

  // Extract the package whole, delta patch included.
  std::filesystem::create_directories(update_dir);
  auto result = ExtractStfsTree(reader, entries, *info, update_dir, nullptr);
  if (!result.complete) {
    REXLOG_ERROR("TU extraction incomplete ({} files written before failure)", result.files);
    return 0;
  }
  REXLOG_INFO("TU extracted: {} files to {}", result.files, update_dir.string());

  // Then place a copy of the delta patch next to the base default.xex.
  std::filesystem::create_directories(xexp_dir);
  auto xexp_path = xexp_dir / "default.xexp";
  {
    std::ofstream ofs(xexp_path, std::ios::binary);
    if (!ofs) {
      REXLOG_ERROR("Cannot write {}", xexp_path.string());
      return 0;
    }
    ofs.write(xexp_data.data(), static_cast<std::streamsize>(xexp_data.size()));
    if (!ofs) {
      REXLOG_ERROR("Failed writing {}", xexp_path.string());
      return 0;
    }
  }
  REXLOG_INFO("Delta patch copied to {}", xexp_path.string());

  return result.files;
}

// ---------------------------------------------------------------------------
// Native dialog helpers
// ---------------------------------------------------------------------------

/// Check that default.xex exists at dir and, if expected is non-empty, that
/// its SHA-256 matches.
bool ValidateDefaultXexInDir(const std::filesystem::path& dir, std::string_view expected) {
  auto xex_path = dir / "default.xex";
  if (!std::filesystem::is_regular_file(xex_path)) {
    REXLOG_ERROR("default.xex not found in {}", dir.string());
    return false;
  }
  if (expected.empty()) {
    return true;
  }
  std::string actual = rex::crypto::sha256_file(xex_path);
  if (!HexEqual(actual, expected)) {
    REXLOG_ERROR("default.xex SHA-256 mismatch: expected={}, actual={}", expected, actual);
    return false;
  }
  REXLOG_INFO("default.xex SHA-256 verified OK");
  return true;
}

/// Case-insensitive extension check.
bool HasExtension(const std::filesystem::path& p, std::string_view ext) {
  auto e = p.extension().string();
  for (auto& c : e) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return e == "." + std::string(ext);
}

/// STFS container (XBLA package, title update, ...). Content files inside an
/// unpacked XBLA dump have no extension — GUID-like names such as
/// `EB9399B31E4A9181AA7F8F6B7F6ABD3263DDA24058` — so sniff the magic instead
/// of trusting the file name.
bool IsStfsPackage(const std::filesystem::path& p) {
  FileReader reader(p);
  return reader.ok() && IsStfsMagic(reader);
}

/// XDVDFS disc image, identified by the same partition probe the extractor
/// uses, so sniffing and extraction can never disagree.
bool IsXdvdfsImage(const std::filesystem::path& p) {
  FileReader reader(p);
  return reader.ok() && FindXdvdfs(reader).has_value();
}

/// SDL file-dialog callback stores the first selected path in a caller-owned
/// struct, then signals completion.
/// The callback may run on a different thread than the caller (the Windows
/// IFileOpenDialog backend runs the dialog on its own thread), so `done` is
/// atomic and is published last.
struct FileDialogState {
  std::string path;
  bool error = false;
  std::atomic<bool> done{false};
};

void SDLCALL FileDialogCallback(void* userdata, const char* const* files, int /*filter_index*/) {
  auto* state = static_cast<FileDialogState*>(userdata);
  if (!files) {
    // NULL file list means the dialog failed to run, not that the user
    // cancelled (cancel gives an empty, non-NULL list).
    state->error = true;
  } else if (files[0]) {
    state->path = files[0];
  }
  state->done.store(true, std::memory_order_release);
}

/// Call SDL_ShowOpenFileDialog and pump events until the dialog closes.
/// Handles both synchronous (Windows COM) and async (Linux portal) backends.
bool RunFileDialog(std::string& out_path, std::span<const SDL_DialogFileFilter> filters) {
  FileDialogState state;
  SDL_ShowOpenFileDialog(FileDialogCallback, &state, nullptr, filters.data(),
                         static_cast<int>(filters.size()), nullptr, false);

  // Pump SDL events until the dialog completes.
  // On synchronous platforms (Windows) `done` will already be true here.
  // On async platforms (Linux portal) the callback is triggered by SDL's
  // event loop, so we must pump until it arrives.
  while (!state.done.load(std::memory_order_acquire)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      // Pumping allows the portal callback to fire.
    }
    SDL_Delay(16);
  }

  if (state.error) {
    REXLOG_ERROR("SDL_ShowOpenFileDialog failed: {}", SDL_GetError());
    return false;
  }
  if (state.path.empty()) {
    REXLOG_INFO("File dialog cancelled by user");
    return false;
  }
  out_path = std::move(state.path);
  return true;
}

/// Same as RunFileDialog, but picks a directory instead of a file.
bool RunFolderDialog(std::string& out_path) {
  FileDialogState state;
  SDL_ShowOpenFolderDialog(FileDialogCallback, &state, nullptr, nullptr, false);

  while (!state.done.load(std::memory_order_acquire)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      // Pumping allows the portal callback to fire.
    }
    SDL_Delay(16);
  }

  if (state.error) {
    REXLOG_ERROR("SDL_ShowOpenFolderDialog failed: {}", SDL_GetError());
    return false;
  }
  if (state.path.empty()) {
    REXLOG_INFO("Folder dialog cancelled by user");
    return false;
  }
  out_path = std::move(state.path);
  return true;
}

/// Show a native info message box with up to three buttons and return the
/// index of the one the user clicked (-1 on failure). The last button is the
/// escape/cancel action.
/// Note: SDL takes NUL-terminated strings here, so these helpers deliberately
/// take `const std::string&` / `const char*` rather than string_view — a view
/// of a substring would be passed on unterminated.
int ShowInfoBox(const std::string& title, const std::string& message,
                std::span<const char* const> button_labels) {
  SDL_MessageBoxButtonData buttons[3];
  int nbuttons = 0;

  for (const char* label : button_labels) {
    if (!label || !*label || nbuttons == 3) {
      continue;
    }
    buttons[nbuttons] = {0, nbuttons, label};
    ++nbuttons;
  }
  if (nbuttons > 0) {
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[nbuttons - 1].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
  }

  SDL_MessageBoxData mb = {SDL_MESSAGEBOX_INFORMATION,
                           nullptr,
                           title.c_str(),
                           message.c_str(),
                           nbuttons,
                           buttons,
                           nullptr};
  int button_id = -1;
  if (!SDL_ShowMessageBox(&mb, &button_id)) {
    REXLOG_ERROR("SDL_ShowMessageBox failed: {}", SDL_GetError());
    return -1;
  }
  return button_id;
}

/// Show a native error message box.
void ShowErrorBox(const std::string& title, const std::string& message) {
  SDL_MessageBoxButtonData ok = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"};
  SDL_MessageBoxData mb = {
      SDL_MESSAGEBOX_ERROR, nullptr, title.c_str(), message.c_str(), 1, &ok, nullptr};
  int dummy;
  SDL_ShowMessageBox(&mb, &dummy);
}

/// Config file the resolved game_data_root is written back to: the caller's
/// config_path if it set one, otherwise `<exe stem>.toml` next to the
/// executable (which is how ReXApp names it).
std::filesystem::path ResolveConfigPath(const GameDataSelectorSettings& settings) {
  if (!settings.config_path.empty()) {
    return settings.config_path;
  }
  auto exe_path = rex::filesystem::GetExecutablePath();
  if (exe_path.empty()) {
    REXLOG_WARN("No config_path given and the executable path is unknown; not persisting");
    return {};
  }
  return rex::filesystem::GetExecutableFolder() / (exe_path.stem().string() + ".toml");
}

/// Write game_data_root (and update_data_root, if one is in use) back to the
/// config file, patching just those lines and leaving the rest untouched.
void PersistGameDataRoot(const GameDataSelectorSettings& settings) {
  auto config_path = ResolveConfigPath(settings);
  if (config_path.empty()) {
    return;
  }
  std::vector<std::string> names = {"game_data_root"};
  if (!std::string(REXCVAR_GET(update_data_root)).empty()) {
    names.push_back("update_data_root");
  }
  rex::cvar::SaveConfigSubset(config_path, names);
}

}  // namespace

// =============================================================================
// Validation
// =============================================================================

static bool IsGameDataValid(std::string_view game_data_root, std::string_view default_xex_sha256) {
  if (game_data_root.empty()) {
    return false;
  }
  std::filesystem::path dir(game_data_root);
  if (!std::filesystem::is_directory(dir)) {
    return false;
  }
  return ValidateDefaultXexInDir(dir, default_xex_sha256);
}

// =============================================================================
// Title update helper
// =============================================================================

/// Where the title update's files are extracted, to be mounted as the
/// `update:` device. Honours an update_data_root the user has already
/// configured; otherwise defaults to `update` next to the executable.
static std::filesystem::path ResolveUpdateDir() {
  std::string udr = REXCVAR_GET(update_data_root);
  if (!udr.empty()) {
    return std::filesystem::path(udr);
  }
  return rex::filesystem::GetExecutableFolder() / "update";
}

static bool ProcessTitleUpdate(const std::filesystem::path& dir,
                               const GameDataSelectorSettings& settings) {
  auto xexp_path = dir / "default.xexp";
  auto update_dir = ResolveUpdateDir();

  // This build does not patch: a default.xexp left over from a build that did
  // would still be picked up by the loader, so remove the copy next to
  // default.xex. The extracted update tree is left alone — it is the user's
  // data, and switching back to a TU build should not need a re-extraction.
  if (settings.title_update_sha256.empty()) {
    if (std::filesystem::is_regular_file(xexp_path)) {
      std::error_code ec;
      std::filesystem::remove(xexp_path, ec);
      if (ec) {
        REXLOG_WARN("This build needs no title update, but {} could not be removed: {}",
                    xexp_path.string(), ec.message());
      } else {
        REXLOG_INFO("Removed stale {} (this build needs no title update)", xexp_path.string());
      }
    }
    return true;
  }

  // The patch alone is not enough: the extracted update tree is what gets
  // mounted as the update: device, so a missing update dir means the update
  // has to be extracted even though default.xexp is sitting there.
  if (std::filesystem::is_regular_file(xexp_path) && std::filesystem::is_directory(update_dir)) {
    REXLOG_INFO("default.xexp and the extracted update at {} are both present, skipping TU prompt",
                update_dir.string());
    REXCVAR_SET(update_data_root, update_dir.string());
    return true;
  }

  // The patch is missing next to default.xex but the update may already have
  // been extracted on an earlier run — restore the copy from there rather than
  // asking for the package again.
  {
    auto src = update_dir / "default.xexp";
    if (std::filesystem::is_regular_file(src)) {
      std::error_code ec;
      std::filesystem::copy_file(src, xexp_path, std::filesystem::copy_options::overwrite_existing,
                                 ec);
      if (ec) {
        REXLOG_ERROR("Failed to copy {} to {}: {}", src.string(), xexp_path.string(), ec.message());
        // Fall through to the prompt rather than failing outright.
      } else {
        REXLOG_INFO("Restored default.xexp from the extracted update at {}", update_dir.string());
        REXCVAR_SET(update_data_root, update_dir.string());
        return true;
      }
    } else {
      REXLOG_INFO("No extracted update at {}, prompting for the title update package",
                  update_dir.string());
    }
  }

  // Prompt and extract. A wrong pick only costs another trip through the
  // dialog — the already-extracted game files are never at stake here.
  std::string msg =
      "This build of the game requires the official title update to "
      "continue.\n\n"
      "Select the game's title update package file.";

  while (true) {
    const char* const tu_buttons[] = {"Browse...", "Quit"};
    int btn = ShowInfoBox("Title Update Required", msg, tu_buttons);
    if (btn != 0)
      return false;

    std::vector<SDL_DialogFileFilter> tu_filters = {{"All files", "*"}};
    std::string selected;
    if (!RunFileDialog(selected, tu_filters)) {
      // Cancelling the file dialog returns to the prompt, where "Quit" is the
      // way out.
      continue;
    }

    // title_update_sha256 is known non-empty here — an empty one returns above.
    std::string actual = rex::crypto::sha256_file(selected);
    if (!HexEqual(actual, settings.title_update_sha256)) {
      REXLOG_ERROR("TU SHA-256 mismatch: expected={}, actual={}", settings.title_update_sha256,
                   actual);
      ShowErrorBox("SHA-256 Mismatch",
                   "The selected title update does not match the expected "
                   "hash.\n\n"
                   "Please select the correct file.");
      continue;
    }

    REXLOG_INFO("Extracting title update from {}...", selected);
    uint32_t tu_count = ExtractTitleUpdateTo(std::filesystem::path(selected), dir, update_dir);
    if (tu_count == 0) {
      ShowErrorBox("Extraction Failed",
                   "Failed to extract the title update.\n\n"
                   "The file may be damaged or not a valid title update "
                   "package.");
      continue;
    }

    if (!std::filesystem::is_regular_file(xexp_path)) {
      ShowErrorBox("Validation Failed", "The title update did not produce a valid default.xexp.");
      continue;
    }

    REXLOG_INFO("Title update extracted: {} files", tu_count);
    // The extracted tree is what gets mounted as update:; record it so the
    // caller persists it alongside game_data_root.
    REXCVAR_SET(update_data_root, update_dir.string());
    return true;
  }
}

// =============================================================================
// GameDataSelector::EnsureGameData
// =============================================================================

namespace {

bool EnsureGameDataImpl(const GameDataSelectorSettings& settings) {
  // 1. Check whether game_data_root is already valid.
  std::filesystem::path dir;
  {
    std::string gdr = REXCVAR_GET(game_data_root);
    if (!gdr.empty() && IsGameDataValid(gdr, settings.default_xex_sha256)) {
      REXLOG_INFO("game_data_root already valid: {}", gdr);
      dir = std::filesystem::path(gdr);
      // The game files are known-good, so a failed title update means the user
      // quit out of the TU prompt. Re-prompting for the game files here would
      // throw away a perfectly valid extraction.
      if (!ProcessTitleUpdate(dir, settings)) {
        return false;
      }
      PersistGameDataRoot(settings);
      return true;
    }
  }

  // 2. Inform the user what's needed.
  std::string msg = "This recompilation needs the original game files.\n\n";
  if (settings.is_xbla) {
    msg += "Select an Xbox Live Arcade package (XBLA).";
  } else {
    msg += "Select an Xbox 360 game disc (ISO).";
  }
  msg +=
      "\n\nThe files will be extracted automatically.\n"
      "Choose \"Select Folder\" instead to point at an already-extracted "
      "directory containing default.xex.";

  const char* const main_buttons[] = {"Select File...", "Select Folder...", "Quit"};
  int btn = ShowInfoBox("Game Files Required", msg, main_buttons);
  if (btn != 0 && btn != 1) {
    return false;
  }
  const bool pick_folder = (btn == 1);

  // 3. Build the native file-dialog filter list.
  // XBLA content files are extensionless, so "All files" must always be
  // offered — otherwise the user simply can't see the file they need to pick.
  std::vector<SDL_DialogFileFilter> filters;
  if (!settings.is_xbla) {
    filters.push_back({"Xbox 360 Game Disc", "iso"});
  }
  filters.push_back({"All files", "*"});

  // 4. Show the native dialog the user asked for.
  std::string selected;
  bool picked = pick_folder ? RunFolderDialog(selected) : RunFileDialog(selected, filters);
  if (!picked) {
    const char* const ok_button[] = {"OK"};
    ShowInfoBox("Nothing Selected", "No file or folder was selected. The application will exit.",
                ok_button);
    return false;
  }

  std::filesystem::path selected_path(selected);

  // Picking default.xex itself inside an extracted directory is a natural
  // mistake — treat it as selecting the directory that contains it.
  if (std::filesystem::is_regular_file(selected_path) && HasExtension(selected_path, "xex") &&
      selected_path.has_parent_path()) {
    REXLOG_INFO("Selected {}, using its parent directory as the game data root",
                selected_path.filename().string());
    selected_path = selected_path.parent_path();
  }

  // 5. Determine destination for extraction.
  const char* base_path = SDL_GetBasePath();
  std::filesystem::path exe_dir(base_path ? base_path : ".");
  SDL_free(const_cast<char*>(base_path));
  std::filesystem::path out_dir = exe_dir / "assets";

  // 6. Process the selection.
  if (std::filesystem::is_directory(selected_path)) {
    dir = selected_path;
    if (!ValidateDefaultXexInDir(dir, settings.default_xex_sha256)) {
      ShowErrorBox("Validation Failed",
                   "default.xex was not found or its SHA-256 hash did not "
                   "match.\n\n"
                   "Please make sure the directory contains the extracted "
                   "game files.");
      return false;
    }
  } else {
    // File selection: ISO or XBLA. Identify by content, falling back to the
    // extension only when the file can't be sniffed.
    bool is_xbla = IsStfsPackage(selected_path);
    bool is_iso = !is_xbla && IsXdvdfsImage(selected_path);
    if (!is_iso && !is_xbla) {
      is_iso = HasExtension(selected_path, "iso");
      is_xbla = HasExtension(selected_path, "xbla");
    }

    if (!is_iso && !is_xbla) {
      ShowErrorBox("Unsupported File",
                   "The selected file is neither an Xbox 360 disc image "
                   "(XDVDFS) nor an STFS/XBLA package.\n\n"
                   "Please select a valid Xbox 360 game disc or XBLA "
                   "package.");
      return false;
    }

    REXLOG_INFO("Extracting {} to {}...", selected_path.string(), out_dir.string());

    uint32_t file_count = 0;
    if (is_iso) {
      file_count = ExtractIsoTo(selected_path, out_dir);
    } else {
      file_count = ExtractXblaTo(selected_path, out_dir);
    }

    const char* source_desc = is_iso ? "disc image" : "XBLA package";
    if (file_count == 0) {
      ShowErrorBox("Extraction Failed",
                   std::string("Failed to extract the game files.\n\nThe file may be damaged "
                               "or not a valid Xbox 360 ") +
                       source_desc + ".");
      return false;
    }
    REXLOG_INFO("Extraction complete: {} files written", file_count);

    if (!ValidateDefaultXexInDir(out_dir, settings.default_xex_sha256)) {
      ShowErrorBox("Validation Failed",
                   std::string("default.xex was not found or its SHA-256 hash did not "
                               "match after extraction.\n\nThe ") +
                       source_desc + " may be from a different version of the game.");
      return false;
    }
    dir = out_dir;
  }

  // 8. Handle title update.
  if (!ProcessTitleUpdate(dir, settings)) {
    return false;
  }

  REXCVAR_SET(game_data_root, dir.string());
  REXLOG_INFO("Game data set to: {}", dir.string());

  // Persist the resolved root so the wizard only runs once and an
  // already-extracted directory can be used where it lives, instead of being
  // duplicated next to the executable.
  PersistGameDataRoot(settings);
  return true;
}

}  // namespace

bool GameDataSelector::EnsureGameData(const GameDataSelectorSettings& settings) {
  // This runs before any window or logging UI exists, and it touches the
  // filesystem constantly (create_directories, ofstream, copy). An escaped
  // exception here would terminate the process with no explanation at all, so
  // failures are turned into the same message box every other error path uses.
  try {
    return EnsureGameDataImpl(settings);
  } catch (const std::exception& e) {
    REXLOG_ERROR("GameDataSelector: unhandled exception: {}", e.what());
    ShowErrorBox(
        "Setup Failed",
        std::string("Something went wrong while preparing the game files:\n\n") + e.what());
    return false;
  } catch (...) {
    REXLOG_ERROR("GameDataSelector: unhandled non-standard exception");
    ShowErrorBox("Setup Failed", "Something went wrong while preparing the game files.");
    return false;
  }
}

}  // namespace rex::system