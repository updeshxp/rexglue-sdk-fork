/**
 * @file        system/mod_registry.cpp
 * @brief       Shared address/event registry implementation. See
 *              mod_registry.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_registry.h>

#include <rex/logging.h>

namespace rex::system {

ModRegistry::ModRegistry(std::function<bool()> is_patched_provider)
    : is_patched_provider_(std::move(is_patched_provider)) {}

void ModRegistry::RegisterAddress(std::string_view name, uint32_t vanilla_addr, uint32_t tu_addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key(name);
  if (addresses_.contains(key)) {
    REXSYS_WARN("ModRegistry::RegisterAddress: '{}' already registered, ignoring re-registration",
                key);
    return;
  }
  addresses_.emplace(std::move(key), AddressEntry{vanilla_addr, tu_addr});
}

std::optional<uint32_t> ModRegistry::FindAddress(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = addresses_.find(std::string(name));
  if (it == addresses_.end()) {
    return std::nullopt;
  }
  bool patched = is_patched_provider_ && is_patched_provider_();
  return (patched && it->second.tu_addr != 0) ? it->second.tu_addr : it->second.vanilla_addr;
}

void ModRegistry::Subscribe(std::string_view event_name, EventCallback callback) {
  if (!callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  subscribers_[std::string(event_name)].push_back(std::move(callback));
}

void ModRegistry::Publish(std::string_view event_name, const EventPayload& payload) {
  std::vector<EventCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(std::string(event_name));
    if (it == subscribers_.end()) {
      return;
    }
    callbacks = it->second;  // copy: invoke outside the lock
  }
  for (auto& callback : callbacks) {
    callback(payload);
  }
}

void ModRegistry::RegisterTick(TickCallback callback) {
  if (!callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ticks_.push_back(std::move(callback));
}

void ModRegistry::DispatchTick() {
  std::vector<TickCallback> ticks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticks_.empty()) {
      return;
    }
    ticks = ticks_;  // copy: invoke outside the lock
  }
  for (auto& tick : ticks) {
    tick();
  }
}

}  // namespace rex::system
