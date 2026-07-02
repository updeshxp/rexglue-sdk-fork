/**
 * @file        mod_registry_test.cpp
 * @brief       Unit tests for the shared mod address/event/tick registry.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/mod_registry.h>

using rex::system::ModRegistry;

TEST_CASE("ModRegistry resolves vanilla address when tu_addr is 0", "[mod_registry]") {
  ModRegistry registry([] { return true; });  // patched, but no TU address registered
  registry.RegisterAddress("ui.accent_color", 0x1000);

  auto found = registry.FindAddress("ui.accent_color");
  REQUIRE(found.has_value());
  CHECK(*found == 0x1000u);
}

TEST_CASE("ModRegistry resolves vanilla vs TU address via the injected provider",
          "[mod_registry]") {
  bool patched = false;
  ModRegistry registry([&patched] { return patched; });
  registry.RegisterAddress("ui.accent_color", 0x1000, 0x2000);

  patched = false;
  CHECK(*registry.FindAddress("ui.accent_color") == 0x1000u);

  patched = true;
  CHECK(*registry.FindAddress("ui.accent_color") == 0x2000u);
}

TEST_CASE("ModRegistry keeps the first RegisterAddress call for a duplicate name",
          "[mod_registry]") {
  ModRegistry registry;
  registry.RegisterAddress("ui.accent_color", 0x1000);
  registry.RegisterAddress("ui.accent_color", 0x9999);  // ignored, logs a warning

  CHECK(*registry.FindAddress("ui.accent_color") == 0x1000u);
}

TEST_CASE("ModRegistry::FindAddress returns nullopt for an unregistered name", "[mod_registry]") {
  ModRegistry registry;
  CHECK_FALSE(registry.FindAddress("nonexistent").has_value());
}

TEST_CASE("ModRegistry::Publish with no subscribers is a no-op", "[mod_registry]") {
  ModRegistry registry;
  ModRegistry::EventPayload payload;
  payload.u64 = 42;
  CHECK_NOTHROW(registry.Publish("some.event", payload));
}

TEST_CASE("ModRegistry delivers published events to subscribers", "[mod_registry]") {
  ModRegistry registry;
  uint64_t received_u64 = 0;
  double received_f64 = 0.0;
  registry.Subscribe("item.pickup", [&](const ModRegistry::EventPayload& payload) {
    received_u64 = payload.u64;
    received_f64 = payload.f64;
  });

  ModRegistry::EventPayload payload;
  payload.u64 = 7;
  payload.f64 = 3.5;
  registry.Publish("item.pickup", payload);

  CHECK(received_u64 == 7u);
  CHECK(received_f64 == 3.5);
}

TEST_CASE("ModRegistry::DispatchTick fires every registered tick exactly once", "[mod_registry]") {
  ModRegistry registry;
  std::atomic<int> fired_a{0};
  std::atomic<int> fired_b{0};
  registry.RegisterTick([&] { ++fired_a; });
  registry.RegisterTick([&] { ++fired_b; });

  registry.DispatchTick();

  CHECK(fired_a == 1);
  CHECK(fired_b == 1);

  registry.DispatchTick();

  CHECK(fired_a == 2);
  CHECK(fired_b == 2);
}

TEST_CASE("ModRegistry::DispatchTick with no registered ticks is a no-op", "[mod_registry]") {
  ModRegistry registry;
  CHECK_NOTHROW(registry.DispatchTick());
}

TEST_CASE("ModRegistry callbacks may re-enter Subscribe/Publish without deadlocking",
          "[mod_registry]") {
  ModRegistry registry;
  bool reentrant_fired = false;

  // The first subscriber, when invoked, subscribes a second callback to the
  // same event and republishes it. Both calls happen from inside Publish's
  // callback invocation, which must run outside the registry's lock.
  registry.Subscribe("chain.event", [&](const ModRegistry::EventPayload&) {
    registry.Subscribe("chain.event.two",
                       [&](const ModRegistry::EventPayload&) { reentrant_fired = true; });
    registry.Publish("chain.event.two", ModRegistry::EventPayload{});
  });

  CHECK_NOTHROW(registry.Publish("chain.event", ModRegistry::EventPayload{}));
  CHECK(reentrant_fired);
}
