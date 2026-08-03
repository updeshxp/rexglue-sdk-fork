/**
 * @file        core/filesystem_tar.cpp
 * @brief       .tar.gz extraction (gzip via miniz's raw inflate + a minimal
 *              ustar/GNU tar reader). See rex::filesystem::ExtractTarGz in
 *              include/rex/filesystem.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/filesystem.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <miniz.h>

namespace rex::filesystem {

namespace {

// Same rule as filesystem_zip.cpp's IsSafeRelativePath -- archives handled
// here come off the network (auto-update downloads), so entry paths must
// never be trusted to stay inside the archive's own top-level names.
bool IsSafeRelativePath(std::string_view entry_name) {
  if (entry_name.empty()) {
    return false;
  }
  if (entry_name.front() == '/' || entry_name.front() == '\\') {
    return false;
  }
  if (entry_name.size() >= 2 && entry_name[1] == ':') {
    return false;
  }
  size_t start = 0;
  while (start <= entry_name.size()) {
    size_t end = entry_name.find_first_of("/\\", start);
    std::string_view segment =
        entry_name.substr(start, (end == std::string_view::npos ? entry_name.size() : end) - start);
    if (segment == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

// Decompresses a gzip-wrapped (RFC 1952) deflate stream. Parses just enough
// of the header to find where the raw deflate data starts; the trailing
// 8-byte CRC32+ISIZE footer (and anything after it, e.g. from a
// multi-member stream) is left untouched since tinfl's deflate decoder is
// self-terminating. Returns false and populates `error` on a malformed
// header or a decompression failure.
bool GunzipToHeap(const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& error) {
  if (data.size() < 18 || data[0] != 0x1f || data[1] != 0x8b) {
    error = "not a gzip file (bad magic)";
    return false;
  }
  if (data[2] != 8) {
    error = "unsupported gzip compression method";
    return false;
  }
  uint8_t flags = data[3];
  size_t pos = 10;

  if (flags & 0x04) {  // FEXTRA
    if (pos + 2 > data.size()) {
      error = "truncated gzip header (FEXTRA)";
      return false;
    }
    uint16_t xlen = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    pos += 2 + xlen;
  }
  auto skip_cstring = [&](const char* what) -> bool {
    while (pos < data.size() && data[pos] != 0) {
      ++pos;
    }
    if (pos >= data.size()) {
      error = std::string("truncated gzip header (") + what + ")";
      return false;
    }
    ++pos;
    return true;
  };
  if (flags & 0x08 && !skip_cstring("FNAME")) {  // NOLINT
    return false;
  }
  if (flags & 0x10 && !skip_cstring("FCOMMENT")) {  // NOLINT
    return false;
  }
  if (flags & 0x02) {  // FHCRC
    pos += 2;
  }
  if (pos >= data.size()) {
    error = "truncated gzip header";
    return false;
  }

  size_t out_len = 0;
  void* decompressed =
      tinfl_decompress_mem_to_heap(data.data() + pos, data.size() - pos, &out_len, 0);
  if (!decompressed) {
    error = "gzip payload is corrupt (deflate decode failed)";
    return false;
  }
  out.assign(static_cast<uint8_t*>(decompressed), static_cast<uint8_t*>(decompressed) + out_len);
  mz_free(decompressed);
  return true;
}

struct UstarHeader {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char padding[12];
};
static_assert(sizeof(UstarHeader) == 512);

uint64_t ParseOctal(const char* field, size_t len) {
  uint64_t value = 0;
  for (size_t i = 0; i < len && field[i] != 0 && field[i] != ' '; ++i) {
    if (field[i] < '0' || field[i] > '7') {
      break;
    }
    value = value * 8 + static_cast<uint64_t>(field[i] - '0');
  }
  return value;
}

bool IsZeroBlock(const uint8_t* block) {
  for (size_t i = 0; i < 512; ++i) {
    if (block[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ExtractTarGz(const std::filesystem::path& archive, const std::filesystem::path& dest_dir,
                  std::string& error) {
  std::ifstream in(archive, std::ios::binary);
  if (!in) {
    error = "failed to open archive";
    return false;
  }
  std::vector<uint8_t> compressed((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  in.close();

  std::vector<uint8_t> tar;
  if (!GunzipToHeap(compressed, tar, error)) {
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  if (ec) {
    error = "failed to create destination directory: " + ec.message();
    return false;
  }

  size_t pos = 0;
  std::string pending_long_name;  // GNU longname ('L') entry data, if seen.
  while (pos + 512 <= tar.size()) {
    const uint8_t* block = tar.data() + pos;
    if (IsZeroBlock(block)) {
      break;  // end-of-archive marker.
    }
    UstarHeader header;
    std::memcpy(&header, block, sizeof(UstarHeader));
    pos += 512;

    uint64_t size = ParseOctal(header.size, sizeof(header.size));
    size_t data_blocks = (size + 511) / 512;
    if (pos + data_blocks * 512 > tar.size()) {
      error = "truncated tar archive";
      return false;
    }
    const uint8_t* entry_data = tar.data() + pos;
    pos += data_blocks * 512;

    if (header.typeflag == 'L') {
      // GNU long-name extension: this entry's data *is* the next entry's
      // real name (NUL-terminated), which replaces the truncated one in its
      // own 100-byte `name` field.
      pending_long_name.assign(reinterpret_cast<const char*>(entry_data),
                               strnlen(reinterpret_cast<const char*>(entry_data), size));
      continue;
    }
    if (header.typeflag == 'x' || header.typeflag == 'g') {
      // pax extended header -- not parsed, just skipped (best-effort; the
      // ustar `name`/`size` fields on the following real entry are used
      // as-is, same as tar implementations that ignore pax records).
      continue;
    }

    std::string name;
    if (!pending_long_name.empty()) {
      name = pending_long_name;
      pending_long_name.clear();
    } else {
      size_t prefix_len = strnlen(header.prefix, sizeof(header.prefix));
      size_t name_len = strnlen(header.name, sizeof(header.name));
      if (prefix_len > 0) {
        name.assign(header.prefix, prefix_len);
        name += '/';
        name.append(header.name, name_len);
      } else {
        name.assign(header.name, name_len);
      }
    }
    if (name.empty()) {
      continue;
    }
    if (!IsSafeRelativePath(name)) {
      error = "archive entry has an unsafe path: " + name;
      return false;
    }

    std::filesystem::path dest_path = dest_dir / rex::to_path(name);
    if (header.typeflag == '5') {  // directory
      std::filesystem::create_directories(dest_path, ec);
      continue;
    }
    if (header.typeflag != '0' && header.typeflag != '\0') {
      continue;  // symlink/device/fifo/etc -- not needed for a game install.
    }

    std::filesystem::create_directories(dest_path.parent_path(), ec);
    if (ec) {
      error = "failed to create directory for " + name + ": " + ec.message();
      return false;
    }
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "failed to write " + dest_path.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(entry_data), static_cast<std::streamsize>(size));
  }

  return true;
}

}  // namespace rex::filesystem
