/**
 * @file        system/mod_storage.h
 * @brief       Small persistent key/value store for mods
 *
 * @remarks     Every mod that wants to remember something across launches
 *              (a toggle, a custom value the built-in Settings overlay
 *              doesn't cover) otherwise hand-rolls its own line-based
 *              key=value file parser/writer -- see
 *              mods_src/graphics_settings's ConfigFilePath/
 *              LoadPersistedRatios/SavePersistedRatios for a worked example
 *              of exactly that boilerplate. ModStorage is that boilerplate,
 *              factored out once. It intentionally mirrors the on-disk
 *              format graphics_settings already uses (plain "key=value"
 *              lines) rather than inventing a new one.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rex::system {

class ModStorage {
 public:
  // |path| is typically Runtime::user_data_root() / "mods" / "<mod_name>.cfg"
  // -- the same per-game folder the game's own saves go under, not the
  // shared nocturnerecomp.toml cvar config (see graphics_settings's
  // ConfigFilePath comment for why: this survives independently of that
  // file's own save/load lifecycle).
  explicit ModStorage(std::filesystem::path path);

  // Reads the backing file into memory. Safe to call even if the file
  // doesn't exist yet (returns false, leaves the store empty rather than
  // treating a fresh install as an error).
  bool Load();

  // Writes the current contents to the backing file, creating parent
  // directories as needed. Returns false if the file couldn't be opened for
  // writing.
  bool Save() const;

  std::optional<std::string> GetString(std::string_view key) const;
  void SetString(std::string_view key, std::string value);

  std::optional<double> GetDouble(std::string_view key) const;
  void SetDouble(std::string_view key, double value);

  std::optional<int64_t> GetInt(std::string_view key) const;
  void SetInt(std::string_view key, int64_t value);

  bool Contains(std::string_view key) const;
  void Erase(std::string_view key);

 private:
  std::filesystem::path path_;
  std::unordered_map<std::string, std::string> values_;
};

}  // namespace rex::system
