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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/crypto/sha256.h>
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

struct XdvdfsInfo {
  uint64_t game_offset;
  uint64_t root_offset;
  uint32_t root_size;
};

std::optional<XdvdfsInfo> FindXdvdfs(std::span<const uint8_t> data) {
  static constexpr std::string_view kMagic = "MICROSOFT*XBOX*MEDIA";
  static constexpr uint32_t kOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                          0x0FD90000};
  for (uint32_t game_offset : kOffsets) {
    uint64_t magic_off = static_cast<uint64_t>(game_offset) + 32 * 2048;
    if (magic_off + kMagic.size() > data.size()) {
      continue;
    }
    if (std::memcmp(data.data() + magic_off, kMagic.data(), kMagic.size()) != 0) {
      continue;
    }
    uint64_t fs_off = magic_off;
    uint32_t root_sector, root_size;
    std::memcpy(&root_sector, data.data() + fs_off + 20, 4);
    std::memcpy(&root_size, data.data() + fs_off + 24, 4);
    if (root_size < 13 || root_size > 32 * 1024 * 1024) {
      continue;
    }
    uint64_t root_offset =
        static_cast<uint64_t>(game_offset) + static_cast<uint64_t>(root_sector) * 2048;
    return XdvdfsInfo{static_cast<uint64_t>(game_offset), root_offset, root_size};
  }
  return std::nullopt;
}

uint32_t ExtractEntry(std::span<const uint8_t> data, uint64_t game_offset, uint64_t buffer_offset,
                      uint32_t ordinal, const std::filesystem::path& out_dir, uint32_t count) {
  uint64_t p = buffer_offset + static_cast<uint64_t>(ordinal) * 4;
  if (p + 14 > data.size()) {
    return count;
  }

  uint16_t node_l, node_r;
  uint32_t sector, length;
  uint8_t attributes, name_length;

  std::memcpy(&node_l, data.data() + p, 2);
  std::memcpy(&node_r, data.data() + p + 2, 2);
  std::memcpy(&sector, data.data() + p + 4, 4);
  std::memcpy(&length, data.data() + p + 8, 4);
  attributes = data[p + 12];
  name_length = data[p + 13];

  if (p + 14 + name_length > data.size()) {
    return count;
  }
  std::string_view name(reinterpret_cast<const char*>(data.data() + p + 14), name_length);

  if (node_l) {
    count = ExtractEntry(data, game_offset, buffer_offset, node_l, out_dir, count);
  }

  auto entry_path = out_dir / std::filesystem::path(name);

  if (attributes & 0x10) {
    std::filesystem::create_directories(entry_path);
    if (length) {
      uint64_t child_offset = game_offset + static_cast<uint64_t>(sector) * 2048;
      count = ExtractEntry(data, game_offset, child_offset, 0, entry_path, count);
    }
  } else {
    uint64_t file_offset = game_offset + static_cast<uint64_t>(sector) * 2048;
    if (file_offset + length <= data.size()) {
      std::ofstream ofs(entry_path, std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(data.data() + file_offset), length);
    }
    ++count;
  }

  if (node_r) {
    count = ExtractEntry(data, game_offset, buffer_offset, node_r, out_dir, count);
  }

  return count;
}

uint32_t ExtractIsoTo(const std::filesystem::path& iso_path, const std::filesystem::path& out_dir) {
  std::ifstream ifs(iso_path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    REXLOG_ERROR("Failed to open ISO: {}", iso_path.string());
    return 0;
  }

  std::streamsize file_size = ifs.tellg();
  if (file_size <= 0) {
    REXLOG_ERROR("Empty ISO: {}", iso_path.string());
    return 0;
  }
  if (file_size > 16LL * 1024 * 1024 * 1024) {
    REXLOG_ERROR("ISO too large ({} bytes): {}", file_size, iso_path.string());
    return 0;
  }

  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  ifs.seekg(0, std::ios::beg);
  if (!ifs.read(reinterpret_cast<char*>(data.data()), file_size)) {
    REXLOG_ERROR("Failed to read ISO: {}", iso_path.string());
    return 0;
  }

  auto info = FindXdvdfs(data);
  if (!info) {
    REXLOG_ERROR("Not a valid XDVDFS image: {}", iso_path.string());
    return 0;
  }

  std::filesystem::create_directories(out_dir);
  return ExtractEntry(data, info->game_offset, info->root_offset, 0, out_dir, 0);
}

// =============================================================================
// STFS (LIVE package) helpers — used by XBLA extraction
// =============================================================================

struct StfsEntry {
  std::string name;
  bool is_directory;
  int blocks;
  int start_block;
  int parent;
  int size;
};

int StfsPhysicalBlock(int logical_block) {
  int group = logical_block / 0xAA;
  int level1_groups = group / 0xAA;
  int level1_overhead = level1_groups > 0 ? level1_groups + 1 : 0;
  return logical_block + 0x0C + group + (group > 0 ? 1 : 0) + level1_overhead;
}

int64_t StfsHashEntryOffset(int logical_block) {
  int group = logical_block / 0xAA;
  int idx = logical_block % 0xAA;
  int level1_groups = group / 0xAA;
  int level1_overhead = level1_groups > 0 ? level1_groups + 1 : 0;
  int table_block = 0x0B + (group * 0xAB) + (group > 0 ? 1 : 0) + level1_overhead;
  return static_cast<int64_t>(table_block) * 0x1000 + (idx * 0x18);
}

std::vector<StfsEntry> ParseStfsEntries(std::ifstream& ifs) {
  std::vector<StfsEntry> entries;
  ifs.seekg(0xC000);
  while (true) {
    uint8_t raw[0x40];
    if (!ifs.read(reinterpret_cast<char*>(raw), sizeof(raw)))
      break;

    bool all_zero = true;
    for (auto b : raw) {
      if (b) {
        all_zero = false;
        break;
      }
    }
    if (all_zero)
      break;

    uint8_t name_flags = raw[0x28];
    int name_len = name_flags & 0x3F;
    if (name_len == 0 || name_len > 0x28)
      break;

    std::string name(reinterpret_cast<const char*>(raw), name_len);
    bool is_dir = (name_flags & 0x80) != 0;

    int blocks = raw[0x29] | (raw[0x2A] << 8) | (raw[0x2B] << 16);
    int start_block = raw[0x2F] | (raw[0x30] << 8) | (raw[0x31] << 16);
    int parent = (static_cast<int>(raw[0x32]) << 8) | raw[0x33];
    int size = (static_cast<int>(raw[0x34]) << 24) | (static_cast<int>(raw[0x35]) << 16) |
               (static_cast<int>(raw[0x36]) << 8) | raw[0x37];

    entries.push_back({std::move(name), is_dir, blocks, start_block, parent, size});
  }
  return entries;
}

bool ExtractStfsFile(std::ifstream& ifs, const StfsEntry& entry,
                     const std::filesystem::path& dest) {
  std::filesystem::create_directories(dest.parent_path());
  std::ofstream ofs(dest, std::ios::binary);
  if (!ofs)
    return false;

  int remaining = entry.size;
  int needed_blocks = (entry.size + 0xFFF) / 0x1000;
  int blocks_to_copy = entry.blocks > needed_blocks ? entry.blocks : needed_blocks;
  int logical_block = entry.start_block;

  for (int bi = 0; bi < blocks_to_copy; ++bi) {
    if (remaining <= 0)
      break;
    if (logical_block == 0xFFFFFF)
      return false;

    int64_t phys_off = static_cast<int64_t>(StfsPhysicalBlock(logical_block)) * 0x1000;
    ifs.seekg(phys_off);

    int to_read = remaining < 0x1000 ? remaining : 0x1000;
    uint8_t buf[0x1000];
    if (!ifs.read(reinterpret_cast<char*>(buf), to_read))
      return false;
    ofs.write(reinterpret_cast<const char*>(buf), to_read);
    remaining -= to_read;

    if (bi + 1 < blocks_to_copy) {
      ifs.seekg(StfsHashEntryOffset(logical_block) + 0x15);
      uint8_t next[3];
      if (!ifs.read(reinterpret_cast<char*>(next), 3))
        return false;
      logical_block =
          (static_cast<int>(next[0]) << 16) | (static_cast<int>(next[1]) << 8) | next[2];
    }
  }
  return true;
}

uint32_t ExtractXblaTo(const std::filesystem::path& xbla_path,
                       const std::filesystem::path& out_dir) {
  std::ifstream ifs(xbla_path, std::ios::binary);
  if (!ifs) {
    REXLOG_ERROR("Failed to open XBLA: {}", xbla_path.string());
    return 0;
  }

  char magic[4];
  if (!ifs.read(magic, 4) || std::memcmp(magic, "LIVE", 4) != 0) {
    REXLOG_ERROR("Not a LIVE/STFS package: {}", xbla_path.string());
    return 0;
  }

  auto entries = ParseStfsEntries(ifs);
  if (entries.empty()) {
    REXLOG_ERROR("No STFS entries found in XBLA: {}", xbla_path.string());
    return 0;
  }

  REXLOG_INFO("Extracting {} STFS entries from {}", entries.size(), xbla_path.string());

  bool found_default = false;
  uint32_t count = 0;
  std::filesystem::create_directories(out_dir);

  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];

    // Build path from parent chain.
    std::vector<std::string> parts = {entry.name};
    int p = entry.parent;
    int depth = 0;
    while (p != 0xFFFF) {
      if (p < 0 || static_cast<size_t>(p) >= entries.size() || depth > 100) {
        break;
      }
      parts.push_back(entries[p].name);
      p = entries[p].parent;
      ++depth;
    }
    if (p != 0xFFFF) {
      REXLOG_WARN("Invalid parent chain for entry '{}'", entry.name);
      continue;
    }

    std::filesystem::path rel;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      rel /= *it;
    }

    auto dest = out_dir / rel;

    if (entry.is_directory) {
      std::filesystem::create_directories(dest);
    } else {
      if (ExtractStfsFile(ifs, entry, dest)) {
        ++count;
        if (entry.name == "default.xex" && entry.parent == 0xFFFF) {
          found_default = true;
        }
      }
    }
  }

  if (!found_default) {
    REXLOG_ERROR("XBLA extraction did not produce a root default.xex");
    return 0;
  }

  return count;
}

// =============================================================================
// Title-update STFS container helpers — LIVE/CON/PIRS packages with dynamic
// hash levels. Ported from scripts/extract_tu.py.
// =============================================================================

struct StfsBlockInfo {
  int header_size;
  bool read_only_format;
  int file_table_block_count;
  int file_table_block_number;
  int64_t data_base;
  int blocks_per_hash_table;
  int block_step[2];
  int total_block_count;
};

static constexpr int kHashLevel0 = 170;
static constexpr int kHashLevel1 = 28900;    // 170 * 170
static constexpr int kHashLevel2 = 4913000;  // 170 * 170 * 170

StfsBlockInfo ParseStfsBlockInfo(const std::vector<uint8_t>& data) {
  StfsBlockInfo info{};
  info.header_size = (static_cast<int>(data[0x340]) << 24) | (static_cast<int>(data[0x341]) << 16) |
                     (static_cast<int>(data[0x342]) << 8) | data[0x343];

  int vd = 0x379;
  uint8_t flags = data[vd + 2];
  info.read_only_format = (flags & 0x1) != 0;
  info.file_table_block_count = data[vd + 3] | (static_cast<int>(data[vd + 4]) << 8);
  info.file_table_block_number =
      data[vd + 5] | (static_cast<int>(data[vd + 6]) << 8) | (static_cast<int>(data[vd + 7]) << 16);
  info.total_block_count = (static_cast<int>(data[vd + 0x1C]) << 24) |
                           (static_cast<int>(data[vd + 0x1D]) << 16) |
                           (static_cast<int>(data[vd + 0x1E]) << 8) | data[vd + 0x1F];

  info.blocks_per_hash_table = info.read_only_format ? 1 : 2;
  info.block_step[0] = kHashLevel0 + info.blocks_per_hash_table;
  info.block_step[1] = kHashLevel1 + (kHashLevel0 + 1) * info.blocks_per_hash_table;
  info.data_base = ((info.header_size + 0xFFF) / 0x1000) * 0x1000;
  return info;
}

int64_t StfsBlockToOffset(int block_index, int64_t data_base, int blocks_per_hash_table) {
  int64_t block = block_index;
  int base = kHashLevel0;
  for (int i = 0; i < 3; ++i) {
    block += ((block_index + base) / base) * blocks_per_hash_table;
    if (block_index < base)
      break;
    base *= kHashLevel0;
  }
  return data_base + (block << 12);
}

int StfsHashBlockNumber(int block_index, int level, int blocks_per_hash_table,
                        const int* block_step) {
  if (level == 0) {
    if (block_index < kHashLevel0)
      return 0;
    int block = (block_index / kHashLevel0) * block_step[0];
    block += ((block_index / kHashLevel1) + 1) * blocks_per_hash_table;
    if (block_index < kHashLevel1)
      return block;
    return block + blocks_per_hash_table;
  }
  if (level == 1) {
    if (block_index < kHashLevel1)
      return block_step[0];
    int block = (block_index / kHashLevel1) * block_step[1];
    return block + blocks_per_hash_table;
  }
  return block_step[1];
}

int NextStfsBlock(int block_index, const std::vector<uint8_t>& data, const StfsBlockInfo& info) {
  int secondary = info.read_only_format ? 0 : 0x1000;

  int hb0 = StfsHashBlockNumber(block_index, 0, info.blocks_per_hash_table, info.block_step);
  int64_t off0 = info.data_base + (static_cast<int64_t>(hb0) << 12);

  if (info.read_only_format) {
    int rec = block_index % kHashLevel0;
    int entry_off = static_cast<int>(off0 + rec * 0x18 + 0x14);
    if (entry_off + 4 > static_cast<int>(data.size()))
      return 0xFFFFFF;
    uint32_t info_word;
    std::memcpy(&info_word, data.data() + entry_off, sizeof(info_word));
    return static_cast<int>(info_word & 0xFFFFFF);
  }

  if (info.total_block_count > kHashLevel1) {
    int hb2 = StfsHashBlockNumber(block_index, 2, info.blocks_per_hash_table, info.block_step);
    int64_t off2 = info.data_base + (static_cast<int64_t>(hb2) << 12) + secondary;
    int rec2 = (block_index / kHashLevel1) % kHashLevel0;
    uint32_t info2;
    std::memcpy(&info2, data.data() + off2 + rec2 * 0x18 + 0x14, sizeof(info2));
    secondary = (info2 & 0x40000000) ? 0x1000 : 0;
  }

  int hb1 = StfsHashBlockNumber(block_index, 1, info.blocks_per_hash_table, info.block_step);
  int64_t off1 = info.data_base + (static_cast<int64_t>(hb1) << 12) + secondary;
  int rec1 = (block_index / kHashLevel0) % kHashLevel0;
  uint32_t info1;
  std::memcpy(&info1, data.data() + off1 + rec1 * 0x18 + 0x14, sizeof(info1));
  secondary = (info1 & 0x40000000) ? 0x1000 : 0;

  int rec0 = block_index % kHashLevel0;
  int64_t off = off0 + secondary;
  uint32_t info_word;
  std::memcpy(&info_word, data.data() + off + rec0 * 0x18 + 0x14, sizeof(info_word));
  return static_cast<int>(info_word & 0xFFFFFF);
}

std::vector<StfsEntry> ParseStfsFileTable(const std::vector<uint8_t>& data,
                                          const StfsBlockInfo& info) {
  std::vector<StfsEntry> entries;
  int table_block = info.file_table_block_number;
  for (int bi = 0; bi < info.file_table_block_count; ++bi) {
    if (table_block == 0xFFFFFF)
      break;
    int64_t block_off = StfsBlockToOffset(table_block, info.data_base, info.blocks_per_hash_table);

    for (int m = 0; m < 0x1000 / 0x40; ++m) {
      int entry_off = static_cast<int>(block_off) + m * 0x40;
      if (entry_off + 0x40 > static_cast<int>(data.size()))
        break;

      const uint8_t* raw = data.data() + entry_off;
      if (raw[0] == 0)
        break;

      uint8_t name_flags = raw[0x28];
      int name_len = name_flags & 0x3F;
      if (name_len == 0)
        break;

      std::string name(reinterpret_cast<const char*>(raw), name_len);
      bool is_dir = (name_flags & 0x80) != 0;

      int allocated =
          raw[0x2C] | (static_cast<int>(raw[0x2D]) << 8) | (static_cast<int>(raw[0x2E]) << 16);
      int start_block =
          raw[0x2F] | (static_cast<int>(raw[0x30]) << 8) | (static_cast<int>(raw[0x31]) << 16);
      int parent = (static_cast<int>(raw[0x32]) << 8) | raw[0x33];
      int size = (static_cast<int>(raw[0x34]) << 24) | (static_cast<int>(raw[0x35]) << 16) |
                 (static_cast<int>(raw[0x36]) << 8) | raw[0x37];

      entries.push_back({std::move(name), is_dir, allocated, start_block, parent, size});
    }

    table_block = NextStfsBlock(table_block, data, info);
  }
  return entries;
}

bool StfsReadFile(const std::vector<uint8_t>& data, const StfsEntry& entry,
                  const StfsBlockInfo& info, std::vector<uint8_t>& out) {
  out.clear();
  int block_index = entry.start_block;
  int remaining = entry.size;
  out.reserve(static_cast<size_t>(remaining));

  while (remaining > 0 && block_index != 0xFFFFFF) {
    int64_t block_off = StfsBlockToOffset(block_index, info.data_base, info.blocks_per_hash_table);
    int to_read = remaining < 0x1000 ? remaining : 0x1000;
    auto off = static_cast<size_t>(block_off);
    if (off + static_cast<size_t>(to_read) > data.size())
      return false;
    out.insert(out.end(), data.begin() + static_cast<int64_t>(off),
               data.begin() + static_cast<int64_t>(off) + to_read);
    remaining -= to_read;
    if (remaining > 0) {
      block_index = NextStfsBlock(block_index, data, info);
    }
  }
  return remaining == 0;
}

uint32_t ExtractTitleUpdateTo(const std::filesystem::path& tu_path,
                              const std::filesystem::path& out_dir) {
  std::ifstream ifs(tu_path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    REXLOG_ERROR("Failed to open TU: {}", tu_path.string());
    return 0;
  }

  std::streamsize file_size = ifs.tellg();
  if (file_size <= 0 || file_size > 256LL * 1024 * 1024) {
    REXLOG_ERROR("TU invalid or too large ({} bytes): {}", file_size, tu_path.string());
    return 0;
  }

  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  ifs.seekg(0);
  if (!ifs.read(reinterpret_cast<char*>(data.data()), file_size)) {
    REXLOG_ERROR("Failed to read TU: {}", tu_path.string());
    return 0;
  }

  auto magic_ok = [&] {
    if (data.size() < 4)
      return false;
    return std::memcmp(data.data(), "CON ", 4) == 0 || std::memcmp(data.data(), "LIVE", 4) == 0 ||
           std::memcmp(data.data(), "PIRS", 4) == 0;
  };
  if (!magic_ok()) {
    REXLOG_ERROR("Not an STFS package: {}", tu_path.string());
    return 0;
  }

  uint32_t volume_type;
  std::memcpy(&volume_type, data.data() + 0x3A9, sizeof(volume_type));
  if (volume_type != 0) {
    REXLOG_ERROR("SVOD packages not supported: {}", tu_path.string());
    return 0;
  }

  auto info = ParseStfsBlockInfo(data);
  auto entries = ParseStfsFileTable(data, info);
  if (entries.empty()) {
    REXLOG_ERROR("No entries found in TU: {}", tu_path.string());
    return 0;
  }
  REXLOG_INFO("TU has {} entries", entries.size());

  const StfsEntry* xexp_entry = nullptr;
  size_t max_size = 0;
  for (const auto& e : entries) {
    if (!e.is_directory && static_cast<size_t>(e.size) > max_size) {
      max_size = e.size;
      xexp_entry = &e;
    }
  }
  if (!xexp_entry) {
    REXLOG_ERROR("No files found in TU: {}", tu_path.string());
    return 0;
  }

  std::vector<uint8_t> xexp_data;
  if (!StfsReadFile(data, *xexp_entry, info, xexp_data)) {
    REXLOG_ERROR("Failed to read XEX2 delta from TU: {}", tu_path.string());
    return 0;
  }
  if (xexp_data.size() < 4 || std::memcmp(xexp_data.data(), "XEX2", 4) != 0) {
    REXLOG_ERROR("Largest file in TU is not an XEX2: {}", tu_path.string());
    return 0;
  }

  std::filesystem::create_directories(out_dir);
  {
    std::ofstream ofs(out_dir / "default.xexp", std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(xexp_data.data()), xexp_data.size());
  }
  uint32_t count = 1;

  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    if (e.is_directory)
      continue;
    if (&e == xexp_entry)
      continue;

    std::vector<std::string> parts = {e.name};
    int p = e.parent;
    int depth = 0;
    while (p != 0xFFFF) {
      if (p < 0 || static_cast<size_t>(p) >= entries.size() || depth > 100)
        break;
      parts.push_back(entries[p].name);
      p = entries[p].parent;
      ++depth;
    }
    if (p != 0xFFFF) {
      REXLOG_WARN("Invalid parent chain for entry '{}'", e.name);
      continue;
    }

    std::filesystem::path rel;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
      rel /= *it;

    auto dest = out_dir / rel;
    std::filesystem::create_directories(dest.parent_path());

    std::vector<uint8_t> file_data;
    if (!StfsReadFile(data, e, info, file_data)) {
      REXLOG_WARN("Failed to read file '{}'", e.name);
      continue;
    }
    {
      std::ofstream ofs(dest, std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(file_data.data()), file_data.size());
    }
    ++count;
  }

  return count;
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

/// SDL file-dialog callback stores the first selected path in a caller-owned
/// struct, then signals completion.
struct FileDialogState {
  std::string path;
  bool done = false;
};

void SDLCALL FileDialogCallback(void* userdata, const char* const* files, int /*filter_index*/) {
  auto* state = static_cast<FileDialogState*>(userdata);
  if (files && files[0]) {
    state->path = files[0];
  }
  state->done = true;
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
  while (!state.done) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      // Pumping allows the portal callback to fire.
    }
    SDL_Delay(16);
  }

  if (state.path.empty()) {
    return false;
  }
  out_path = std::move(state.path);
  return true;
}

/// Show a native info message box and return the button index the user clicked.
int ShowInfoBox(std::string_view title, std::string_view message, std::string_view button_ok,
                std::string_view button_cancel) {
  SDL_MessageBoxButtonData buttons[2];
  int nbuttons = 0;

  if (!button_cancel.empty()) {
    buttons[nbuttons] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, nbuttons, button_cancel.data()};
    ++nbuttons;
  }
  if (!button_ok.empty()) {
    buttons[nbuttons] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, nbuttons, button_ok.data()};
    ++nbuttons;
  }

  SDL_MessageBoxData mb = {SDL_MESSAGEBOX_INFORMATION,
                           nullptr,
                           title.data(),
                           message.data(),
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
void ShowErrorBox(std::string_view title, std::string_view message) {
  SDL_MessageBoxButtonData ok = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"};
  SDL_MessageBoxData mb = {
      SDL_MESSAGEBOX_ERROR, nullptr, title.data(), message.data(), 1, &ok, nullptr};
  int dummy;
  SDL_ShowMessageBox(&mb, &dummy);
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

static bool ProcessTitleUpdate(const std::filesystem::path& dir,
                               const GameDataSelectorSettings& settings) {
  if (settings.title_update_mode == TitleUpdateMode::None)
    return true;

  auto xexp_path = dir / "default.xexp";
  if (std::filesystem::is_regular_file(xexp_path)) {
    REXLOG_INFO("default.xexp already present, skipping TU prompt");
    return true;
  }

  // Optional mode only prompts during fresh extraction (inline in
  // EnsureGameData), never on subsequent launches.
  if (settings.title_update_mode == TitleUpdateMode::Optional)
    return true;

  // Required mode: prompt and extract.
  std::string msg =
      "This recompilation requires a title update to continue.\n\n"
      "Select the Xbox 360 system update package file.";
  int btn = ShowInfoBox("Title Update Required", msg, "Browse...", "Quit");
  if (btn != 0)
    return false;

  std::vector<SDL_DialogFileFilter> tu_filters = {{"All files (*)", "*"}};
  std::string selected;
  if (!RunFileDialog(selected, tu_filters)) {
    ShowErrorBox("Title Update Required",
                 "No title update was selected.\n"
                 "The application will exit.");
    return false;
  }

  if (!settings.title_update_sha256.empty()) {
    std::string actual = rex::crypto::sha256_file(selected);
    if (!HexEqual(actual, settings.title_update_sha256)) {
      REXLOG_ERROR("TU SHA-256 mismatch: expected={}, actual={}", settings.title_update_sha256,
                   actual);
      ShowErrorBox("SHA-256 Mismatch",
                   "The selected title update does not match the expected "
                   "hash.\n\n"
                   "Please select the correct file.");
      return false;
    }
  }

  REXLOG_INFO("Extracting title update from {}...", selected);
  uint32_t tu_count = ExtractTitleUpdateTo(std::filesystem::path(selected), dir);
  if (tu_count == 0) {
    ShowErrorBox("Extraction Failed",
                 "Failed to extract the title update.\n\n"
                 "The file may be damaged or not a valid title update "
                 "package.");
    return false;
  }

  if (!std::filesystem::is_regular_file(xexp_path)) {
    ShowErrorBox("Validation Failed", "The title update did not produce a valid default.xexp.");
    return false;
  }

  REXLOG_INFO("Title update extracted: {} files", tu_count);
  return true;
}

static void PromptForOptionalTu(const std::filesystem::path& dir,
                                const GameDataSelectorSettings& settings) {
  int btn = ShowInfoBox("Title Update Available",
                        "A title update (Xbox 360 system update) can "
                        "improve compatibility.\n\n"
                        "Would you like to provide one?",
                        "Browse...", "Skip");
  if (btn != 0)
    return;

  std::vector<SDL_DialogFileFilter> tu_filters = {{"All files (*)", "*"}};
  std::string selected;
  if (!RunFileDialog(selected, tu_filters))
    return;

  if (!settings.title_update_sha256.empty()) {
    std::string actual = rex::crypto::sha256_file(selected);
    if (!HexEqual(actual, settings.title_update_sha256)) {
      REXLOG_ERROR("TU SHA-256 mismatch: expected={}, actual={}", settings.title_update_sha256,
                   actual);
      ShowErrorBox("SHA-256 Mismatch",
                   "The selected title update does not match the expected "
                   "hash.");
      return;
    }
  }

  REXLOG_INFO("Extracting title update from {}...", selected);
  uint32_t tu_count = ExtractTitleUpdateTo(std::filesystem::path(selected), dir);
  if (tu_count > 0 && std::filesystem::is_regular_file(dir / "default.xexp")) {
    REXLOG_INFO("Title update extracted: {} files", tu_count);
  } else {
    ShowErrorBox("Extraction Failed", "Failed to extract the title update.");
  }
}

// =============================================================================
// GameDataSelector::EnsureGameData
// =============================================================================

bool GameDataSelector::EnsureGameData(const GameDataSelectorSettings& settings) {
  // 1. Check whether game_data_root is already valid.
  std::filesystem::path dir;
  {
    std::string gdr = REXCVAR_GET(game_data_root);
    if (!gdr.empty() && IsGameDataValid(gdr, settings.default_xex_sha256)) {
      REXLOG_INFO("game_data_root already valid: {}", gdr);
      dir = std::filesystem::path(gdr);
      if (ProcessTitleUpdate(dir, settings)) {
        return true;
      }
      // TU handling failed — fall through to re-prompt for everything.
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
      "You can also point to an already-extracted directory containing "
      "default.xex.";

  int btn = ShowInfoBox("Game Files Required", msg, "Browse...", "Quit");
  if (btn != 0) {
    return false;
  }

  // 3. Build the native file-dialog filter list.
  std::vector<SDL_DialogFileFilter> filters;
  if (!settings.is_xbla) {
    filters.push_back({"Xbox 360 Game Disc (*.iso)", "iso"});
  }

  // 4. Show native file-open dialog.
  std::string selected;
  if (!RunFileDialog(selected, filters)) {
    ShowInfoBox("No File Selected", "No file was selected. The application will exit.", "OK", "");
    return false;
  }

  std::filesystem::path selected_path(selected);

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
    // File selection: ISO or XBLA.
    bool is_iso = HasExtension(selected_path, "iso");
    bool is_xbla = HasExtension(selected_path, "xbla");

    if (!is_iso && !is_xbla) {
      ShowErrorBox("Unsupported File",
                   "The selected file does not appear to be an ISO or "
                   "XBLA.\n\n"
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

    if (file_count == 0) {
      ShowErrorBox("Extraction Failed",
                   "Failed to extract the game files.\n\n"
                   "The file may be damaged or not a valid Xbox 360 image.");
      return false;
    }
    REXLOG_INFO("Extraction complete: {} files written", file_count);

    if (!ValidateDefaultXexInDir(out_dir, settings.default_xex_sha256)) {
      ShowErrorBox("Validation Failed",
                   "default.xex was not found or its SHA-256 hash did not "
                   "match after extraction.\n\n"
                   "The ISO may be from a different version of the game.");
      return false;
    }
    dir = out_dir;

    // 7a. For Optional mode, prompt only during fresh extraction.
    if (settings.title_update_mode == TitleUpdateMode::Optional &&
        !std::filesystem::is_regular_file(dir / "default.xexp")) {
      PromptForOptionalTu(dir, settings);
    }
  }

  // 8. Handle title update (Required mode).
  if (!ProcessTitleUpdate(dir, settings)) {
    return false;
  }

  REXCVAR_SET(game_data_root, dir.string());
  REXLOG_INFO("Game data set to: {}", dir.string());
  return true;
}

}  // namespace rex::system