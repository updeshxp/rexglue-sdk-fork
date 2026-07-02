/**
 * @file        system/mod_registry.h
 * @brief       Shared address/event registry mods publish into and consume
 *              from
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     A mod that reverse-engineers a guest address or defines a
 *              semantic event can publish it here instead of every consumer
 *              redoing the work (or copy-pasting constants out of another
 *              mod's source, which doesn't work at all for binary-only
 *              third-party mods). Reached via Runtime::mod_registry(); no
 *              ModHostContext/IModPlugin ABI change, so kModPluginAbiVersion
 *              does not need to bump for this.
 *
 *              Ordering contract: producers register addresses/events from
 *              IModPlugin::OnCreateDialogs (dispatched in enabled_mods
 *              priority order); consumers look up lazily on first use
 *              (typically after OnModuleLaunched), never assuming a specific
 *              dispatch order. A mod.toml `requires` entry (see mod_plugin.h)
 *              is how a consumer guarantees its producer is enabled and
 *              ordered first, rather than relying on convention.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::system {

class ModRegistry {
 public:
  // is_patched_provider resolves vanilla-vs-TU at lookup time (typically
  // wired by Runtime to the executable module's UserModule::is_patched()).
  // An empty provider, or one that returns false, resolves to the vanilla
  // address. Injected rather than reached through a global so the registry
  // is testable without a live KernelState/loaded module.
  explicit ModRegistry(std::function<bool()> is_patched_provider = {});

  ModRegistry(const ModRegistry&) = delete;
  ModRegistry& operator=(const ModRegistry&) = delete;

  // Registers a named guest address. tu_addr == 0 means "use vanilla_addr for
  // both builds." The first registration of a given name wins (the
  // higher-priority mod loads first); a later registration of the same name
  // is ignored and logs a warning, so a misbehaving or duplicate producer
  // can't silently steal a name out from under an earlier one.
  void RegisterAddress(std::string_view name, uint32_t vanilla_addr, uint32_t tu_addr = 0);

  // Resolves a previously registered address via the is_patched_provider.
  // Returns std::nullopt if no mod has registered this name.
  std::optional<uint32_t> FindAddress(std::string_view name) const;

  // Minimal, deliberately trivial event payload. `bytes` is valid only for
  // the duration of the Publish() call that supplied it -- a subscriber that
  // needs to retain the data must copy it before returning.
  struct EventPayload {
    uint64_t u64 = 0;
    double f64 = 0.0;
    std::span<const uint8_t> bytes;
  };
  using EventCallback = std::function<void(const EventPayload&)>;

  // Registers a callback for a named event. Multiple subscribers may share
  // the same event name; all are invoked, in registration order.
  void Subscribe(std::string_view event_name, EventCallback callback);

  // Invokes every subscriber registered for event_name with payload. A
  // publish with no subscribers is a no-op. Callbacks are invoked outside
  // the registry's internal lock, so a callback may itself call
  // Subscribe/Publish/RegisterTick without deadlocking.
  void Publish(std::string_view event_name, const EventPayload& payload);

  // Registers a callback fired once per guest frame (see DispatchTick).
  using TickCallback = std::function<void()>;
  void RegisterTick(TickCallback callback);

  // Fired once per guest frame, on GPU swap (Runtime wires this to
  // GraphicsSystem's host swap callback). Runs on the command-processor
  // thread, not the render/UI thread; keep tick callbacks cheap and aware of
  // that thread. Calling this with no registered ticks is a no-op.
  void DispatchTick();

 private:
  struct AddressEntry {
    uint32_t vanilla_addr = 0;
    uint32_t tu_addr = 0;
  };

  std::function<bool()> is_patched_provider_;

  // Private lock, deliberately not rex::thread::global_critical_region: that
  // is a single process-wide recursive mutex used for kernel-object state,
  // and serializing a per-frame tick against all kernel activity would be
  // the wrong tradeoff for what is otherwise independent mod state.
  mutable std::mutex mutex_;
  std::unordered_map<std::string, AddressEntry> addresses_;
  std::unordered_map<std::string, std::vector<EventCallback>> subscribers_;
  std::vector<TickCallback> ticks_;
};

}  // namespace rex::system
