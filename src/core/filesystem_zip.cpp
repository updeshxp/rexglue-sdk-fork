/**
 * @file        core/filesystem_zip.cpp
 * @brief       ZIP extraction via miniz. See rex::filesystem::ExtractZip in
 *              include/rex/filesystem.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/filesystem.h>

#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#include <miniz.h>

namespace rex::filesystem {

namespace {

// Rejects an absolute path or any ".." path segment. Archive entry names use
// forward slashes regardless of host OS.
bool IsSafeRelativePath(std::string_view entry_name) {
  if (entry_name.empty()) {
    return false;
  }
  if (entry_name.front() == '/' || entry_name.front() == '\\') {
    return false;
  }
  // A drive-letter prefix ("C:") is also absolute on Windows.
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

}  // namespace

bool ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& dest_dir,
                std::string& error) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
    error = "failed to open archive (corrupt or not a zip file)";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  if (ec) {
    mz_zip_reader_end(&zip);
    error = "failed to create destination directory: " + ec.message();
    return false;
  }

  bool ok = true;
  mz_uint file_count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < file_count && ok; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      ok = false;
      error = "failed to read archive entry metadata";
      break;
    }
    std::string_view name(stat.m_filename);
    if (!IsSafeRelativePath(name)) {
      ok = false;
      error = "archive entry has an unsafe path: " + std::string(name);
      break;
    }
    std::filesystem::path dest_path = dest_dir / rex::to_path(name);

    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      std::filesystem::create_directories(dest_path, ec);
      continue;
    }

    std::filesystem::create_directories(dest_path.parent_path(), ec);
    if (ec) {
      ok = false;
      error = "failed to create directory for " + std::string(name) + ": " + ec.message();
      break;
    }

    // Extract this single entry straight to a heap buffer, then write it out
    // -- archives here are mod-sized (tens/low hundreds of MB), not large
    // enough to need a streaming callback API.
    size_t extracted_size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, i, &extracted_size, 0);
    if (!data) {
      ok = false;
      error = "failed to extract " + std::string(name);
      break;
    }
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      mz_free(data);
      ok = false;
      error = "failed to write " + dest_path.string();
      break;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(extracted_size));
    mz_free(data);
  }

  mz_zip_reader_end(&zip);
  return ok;
}

bool MoveOrCopyDirectory(const std::filesystem::path& from, const std::filesystem::path& to,
                         std::string& error) {
  constexpr int kMaxAttempts = 5;
  constexpr auto kRetryDelay = std::chrono::milliseconds(100);

  std::error_code ec;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (!ec) {
      return true;
    }
    if (attempt < kMaxAttempts) {
      std::this_thread::sleep_for(kRetryDelay);
    }
  }

  // Cross-filesystem rename fails every time, not just transiently, so don't
  // burn the same retry budget on it again -- go straight to the copy
  // fallback, which gets its own retry loop below for the same transient-lock
  // reason `rename` above did.
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    ec.clear();
    std::filesystem::create_directories(to, ec);
    if (!ec) {
      std::filesystem::copy(from, to,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
    }
    if (!ec) {
      std::filesystem::remove_all(from, ec);
      return true;
    }
    if (attempt < kMaxAttempts) {
      std::this_thread::sleep_for(kRetryDelay);
    }
  }

  error = "failed to install extracted mod: " + ec.message();
  return false;
}

}  // namespace rex::filesystem
