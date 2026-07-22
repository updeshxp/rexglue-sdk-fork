// rexglue - persistent key/value store for mods. See mod_storage.h.

#include <rex/system/mod_storage.h>

#include <charconv>
#include <cstdlib>
#include <fstream>

#include <rex/filesystem.h>

namespace rex::system {

ModStorage::ModStorage(std::filesystem::path path) : path_(std::move(path)) {}

bool ModStorage::Load() {
  values_.clear();
  std::ifstream file(path_);
  if (!file) {
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    values_[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return true;
}

bool ModStorage::Save() const {
  rex::filesystem::CreateParentFolder(path_);
  std::ofstream file(path_, std::ios::trunc);
  if (!file) {
    return false;
  }
  for (const auto& [key, value] : values_) {
    file << key << "=" << value << "\n";
  }
  return true;
}

std::optional<std::string> ModStorage::GetString(std::string_view key) const {
  auto it = values_.find(std::string(key));
  if (it == values_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void ModStorage::SetString(std::string_view key, std::string value) {
  values_[std::string(key)] = std::move(value);
}

std::optional<double> ModStorage::GetDouble(std::string_view key) const {
  auto raw = GetString(key);
  if (!raw || raw->empty()) {
    return std::nullopt;
  }
  // Deliberately strtod, not std::from_chars<double> -- see
  // rex::cvar::ParseDouble (cvar.h) for the same choice and why.
  char* end = nullptr;
  double value = std::strtod(raw->c_str(), &end);
  if (end == raw->c_str() || *end != '\0') {
    return std::nullopt;
  }
  return value;
}

void ModStorage::SetDouble(std::string_view key, double value) {
  SetString(key, std::to_string(value));
}

std::optional<int64_t> ModStorage::GetInt(std::string_view key) const {
  auto raw = GetString(key);
  if (!raw) {
    return std::nullopt;
  }
  int64_t value = 0;
  auto result = std::from_chars(raw->data(), raw->data() + raw->size(), value);
  if (result.ec != std::errc()) {
    return std::nullopt;
  }
  return value;
}

void ModStorage::SetInt(std::string_view key, int64_t value) {
  SetString(key, std::to_string(value));
}

bool ModStorage::Contains(std::string_view key) const {
  return values_.find(std::string(key)) != values_.end();
}

void ModStorage::Erase(std::string_view key) {
  values_.erase(std::string(key));
}

}  // namespace rex::system
