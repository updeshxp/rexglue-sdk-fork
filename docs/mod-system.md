# Mod system: load order, hierarchy, and dependencies

This documents the SDK's mod system as implemented in `rex::Runtime` and
`rex::system` (`mod_plugin.h`, `mod_registry.h`, `runtime.cpp`). It is
project-agnostic: any downstream recomp project can enable/author mods
against this without SDK changes.

## What a mod is

A mod is a folder under a host-chosen mods root (`mods_data_root` cvar,
default `<exe folder>/mods`). A folder may contain any mix of:

- Asset subfolders (`game/`, `update/`, `dlc/<name>/`, `textures/`,
  `shaders/`) that overlay the corresponding host device or GPU asset cache.
  Purely data; the SDK never inspects their contents beyond the file layout.
- A `code/<stem>.dll` (or `.so`) built against `rex::runtime`, implementing
  `rex::system::IModPlugin`. Loaded once at startup via a versioned C ABI
  (`rex_mod_abi_version`/`rex_mod_create`, see `mod_plugin.h`).
- An optional `mod.toml` manifest (all keys optional; a missing manifest or
  missing field never hard-fails, since asset-only mods commonly omit it
  entirely). Parsed into `rex::system::ModInfo`:

  | Key           | Type                     | Meaning                                                   |
  |---------------|--------------------------|------------------------------------------------------------|
  | `name`        | string                   | Display name (F6-style overlay etc.); defaults to folder name |
  | `version`     | string                   | Display only                                              |
  | `author`      | string                   | Display only                                              |
  | `description` | string                   | Display only                                              |
  | `code`        | string                   | DLL/SO stem under `<mod_root>/code/`; absent = asset-only mod |
  | `requires`    | comma-separated string   | Hard dependency (see below)                                |
  | `load_after`  | comma-separated string   | Soft ordering hint (see below)                             |
  | `conflicts`   | comma-separated string   | Hard mutual exclusion (see below)                          |

  `icon.png` in the mod root, if present, is picked up automatically
  (`ModInfo::icon_path`); not a `mod.toml` key.

## Load order = priority

Which mods are active, and in what order, is entirely driven by one cvar:

```toml
enabled_mods = "mod_a,mod_b,mod_c"
```

A comma-separated list of folder names, trimmed of whitespace, resolved once
by `Runtime::ResolveEnabledMods()` during `Setup()`. **List order is
priority order: index 0 is highest priority.** This single order drives
every part of the system:

- **Asset overlays** (`Runtime::ModOverlayRoots()`): earlier mods win when
  multiple mods ship the same guest-path file, texture hash, or shader.
- **Code-mod lifecycle dispatch**: `IModPlugin::OnCreateDialogs`,
  `OnModuleLaunched`, and `OnShutdown` are called across all loaded mod
  plugins in this same order, for every mod that has a `code` key.
- **Dependency validation** (below): `requires`/`load_after` are checked
  against this same index, not against some separate ordering concept.

A mod folder named in `enabled_mods` but not found on disk is skipped with a
warning, not a hard failure (a config typo shouldn't prevent the game from
starting at all by itself; see `requires` below for when it should).

## The dependency graph: requires / load_after / conflicts

`enabled_mods` order alone can't express "mod B only makes sense if mod A is
also present and loaded first," or "these two mods must never both be
active." Three additional `mod.toml` keys layer a dependency graph on top of
the flat priority list, validated by `Runtime::ValidateModDependencies()`
right after `enabled_mods` is resolved (during `Setup()`, before GPU/graphics
init):

- **`requires = "other_mod"`**: `other_mod` must (a) be present in
  `enabled_mods` and (b) sit at a *lower index* (higher priority, loads
  first). Violating either is a **hard failure**: `Setup()` returns an error
  status naming the mod, the missing/misordered dependency, and the fix
  (e.g. "add 'other_mod' to enabled_mods before 'this_mod'"). This is the
  mechanism that turns "I assume the other mod is enabled and loaded first"
  from an unenforced convention into something the SDK guarantees before
  gameplay code ever runs.
- **`load_after = "other_mod"`**: same order check as `requires`, but only
  **warns** (`REXSYS_WARN`) if violated or if `other_mod` isn't enabled at
  all. `Setup()` still succeeds. Use this for "works better in this order"
  hints that shouldn't block the game from starting.
- **`conflicts = "other_mod"`**: if both this mod and `other_mod` are
  enabled, **hard failure**, regardless of which is listed first.

All three are comma-separated lists of folder names, parsed the same way as
`enabled_mods` itself. A mod naming itself in `requires` or `conflicts` is
also a hard failure (self-reference guard). Note a `requires` cycle (`A`
requires `B`, `B` requires `A`) is already structurally impossible under the
"must sit at a lower index" rule, since both mods can't be ordered before
each other in one list; no separate cycle detector is needed.

## The shared registry (`rex::system::ModRegistry`)

`requires` alone only orders and gates *loading*; it doesn't move data
between mods. `ModRegistry` (reached via `Runtime::mod_registry()`, one
instance per `Runtime`, shared by every mod in the process) is for that: a
flat, named key-value space mods publish reverse-engineered guest addresses
and semantic events into, so that work is done once instead of
re-derived (or copy-pasted) by every mod that needs it.

- `RegisterAddress(name, vanilla_addr, tu_addr)` / `FindAddress(name)`:
  publishes/looks up a guest address by name; resolution of which build's
  address to return happens internally via an injected `is_patched`
  provider, so callers never branch on build variant themselves.
- `Subscribe(event_name, callback)` / `Publish(event_name, payload)`: a
  minimal event bus (payload: a `uint64_t`, a `double`, and a byte span valid
  only for the duration of the `Publish` call).
- `RegisterTick(callback)` / `DispatchTick()`: fired once per guest frame
  (driven by the GPU swap, not the UI/render cadence), on the
  command-processor thread.

**Ordering contract**: a mod that publishes into the registry should do so
from `OnCreateDialogs` (dispatched first, in `enabled_mods` order); a mod
that consumes should look up lazily, on first use (typically from
`OnModuleLaunched` or later), rather than assuming a specific dispatch
order. `requires` is what actually *guarantees* the producer has run by the
time a consumer looks something up; the registry itself has no opinion on
load order beyond "producers register in `OnCreateDialogs`."

Because the registry is one flat namespace shared by every mod, two
producers registering the same name collide: the first registration wins,
and later ones are dropped with a warning. There is no per-mod-isolated
sub-registry; if two independent mods need to publish under the same
conceptual name without colliding, they need distinct key names (e.g. a
mod-specific prefix), the same way two translation units agree not to
define the same global symbol twice.

## `platform` strings (code mods only)

The SDK's own `ModInfo`/`ParseModInfo` does not read or enforce a `platform`
key; it is a convention for a downstream project's *own* mod-build tooling,
worth following if that tooling ships prebuilt per-target binaries (a
`code/<stem>.dll` and a `code/<stem>.so` for the same mod folder, built from
the same source against each target's SDK).

The convention: a `platform` key in a code mod's `mod.toml`, holding a
comma-separated list of target identifiers (e.g. `"windows-x64,linux-x64"`)
recording which platform(s) that mod's `code/` directory currently ships a
binary for. It is written by the build tooling after a build, not by the mod
author, and reflects what's actually on disk right now, not a request or a
restriction to build for that platform.

Asset-only mods (no `code` key) have nothing to record: they ship no native
binary, so there is no per-platform artifact to track, and this key doesn't
apply to them at all.
