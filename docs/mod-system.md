# Mod system: load order, hierarchy, and dependencies

This documents the SDK's mod system as implemented in `rex::Runtime` and
`rex::system` (`mod_plugin.h`, `mod_registry.h`, `runtime.cpp`,
`function_dispatcher.h`). It is project-agnostic: any downstream recomp
project can enable/author mods against this without SDK changes.

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
  | `requires`      | comma-separated string | Hard dependency, each entry optionally `name >= x.y.z` (see below) |
  | `load_after`    | comma-separated string | Soft ordering hint (see below)                             |
  | `conflicts`     | comma-separated string | Hard mutual exclusion (see below)                          |
  | `game_version`  | string                 | Minimum host application version (see below)               |

  `icon.png` in the mod root, if present, is picked up automatically
  (`ModInfo::icon_path`); not a `mod.toml` key.

## Load order = priority

Which mods are active, and in what order, is driven by `<mods_root>/mods.toml`
-- an ordered list of `{ id, enabled }` entries, read/written directly by
`rex::system::ModState` (`include/rex/system/mod_state.h`) and by the in-game
mod manager overlay's "Installed" tab:

```toml
[[mods]]
id = "mod_a"
enabled = true

[[mods]]
id = "mod_b"
enabled = false
```

`ModState::LoadReconciled()` (called from `Runtime::ResolveEnabledMods()`
during `Setup()`) parses this, then reconciles it against what's actually on
disk under the mods root: entries whose folder is gone are dropped, and any
folder not yet recorded is appended at the end as enabled. **List order is
priority order: index 0 is highest priority**; a disabled entry keeps its
slot (so re-enabling it later restores its position instead of dropping it
to the bottom).

If `mods.toml` doesn't exist yet, `Runtime::ResolveEnabledMods()` falls back
to the older `enabled_mods` cvar instead:

```toml
enabled_mods = "mod_a,mod_b,mod_c"
```

A comma-separated list of folder names, trimmed of whitespace. This fallback
exists so a bare command-line launch (no launcher-managed `mods.toml`) still
works; once a `mods.toml` is present, `enabled_mods` is ignored entirely.
Either way, the resolved order drives every part of the system:

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
  first). Violating either is logged with `REXSYS_ERROR` naming the mod, the
  missing/misordered dependency, and the fix (e.g. "add 'other_mod' to
  enabled_mods before 'this_mod'") -- but it's **not fatal**: `Setup()`
  still succeeds. A misconfigured/misordered mod list shouldn't be able to
  stop the game from booting by itself; it's on the mod author or the player
  to notice the logged error and fix the order.

  Each entry can optionally pin a minimum version: `requires = "other_mod >=
  1.0.0"`. The version is compared against `other_mod`'s own `version` key
  (dotted-numeric, e.g. `1.0.0`; missing trailing components count as `0`, so
  `1.0` == `1.0.0`) using `>=` semantics only. This **is a hard failure**
  (`Setup()` returns an error status) when both sides parse and `other_mod`'s
  enabled version is actually older -- the game isn't guaranteed to run
  correctly against a version the mod explicitly rejected. If either side
  can't be checked -- `other_mod` has no `version` key, or the constraint
  itself isn't a valid dotted version -- that's **not** a failure: the mod
  predates this feature (or the dependency does), so the constraint is
  accepted with a `REXSYS_WARN` rather than blocking startup. A bare
  `requires = "other_mod"` (no `>=`) stays unconstrained, same as before this
  existed.
- **`load_after = "other_mod"`**: same order check as `requires`, but only
  **warns** (`REXSYS_WARN`) if violated or if `other_mod` isn't enabled at
  all. `Setup()` still succeeds. Use this for "works better in this order"
  hints that shouldn't block the game from starting.
- **`conflicts = "other_mod"`**: if both this mod and `other_mod` are
  enabled, logged with `REXSYS_ERROR` regardless of which is listed first,
  but not fatal -- `Setup()` still succeeds.

All three are comma-separated lists of folder names, parsed the same way as
`enabled_mods` itself. A mod naming itself in `requires` or `conflicts` is
also logged as an error (self-reference guard) but not fatal. Note a
`requires` cycle (`A` requires `B`, `B` requires `A`) is already structurally
impossible under the "must sit at a lower index" rule, since both mods can't
be ordered before each other in one list; no separate cycle detector is
needed.

Version mismatches are the *only* fatal case here -- everything about
ordering, presence, or conflicts is diagnostic, not a boot blocker.

## Minimum host version (`game_version`)

`requires` pins a mod to another *mod's* version; `game_version` pins it to
the host application's own version instead -- for a mod that relies on a
project's newer API, engine fix, or asset layout, independent of any other
mod:

```toml
game_version = "1.2.0"     # or, equivalently: game_version = ">= 1.2.0"
```

Both forms mean the same thing (minimum version; no other comparison
operator is supported). The host project sets its own current version once,
in `RuntimeConfig::game_version` (e.g. from `OnPreSetup()`):

```cpp
void OnPreSetup(rex::RuntimeConfig& config) override {
  config.game_version = "1.2.0";
}
```

`ValidateModDependencies()` checks every enabled mod's `game_version` against
this at `Setup()` time, alongside `requires`/`conflicts`. It's a **hard
failure** only when both sides parse and the host's version is actually
older than `game_version`. If the host never set `RuntimeConfig::game_version`
at all (or the constraint itself isn't a valid dotted version), the
constraint can't be checked, so it's accepted with a `REXSYS_WARN` rather
than failing -- the same can't-verify-so-don't-block behavior as an
unversioned `requires` dependency with no `version` key. Version comparison
uses the same dotted-numeric scheme as `requires`' `>=` constraints (`1.0` ==
`1.0.0`; missing trailing components count as `0`).

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

## Overriding a game function (`FunctionDispatcher::OverrideFunction`)

Everything above is about assets and data. `FunctionDispatcher` (reached via
`Runtime::instance()->function_dispatcher()`) is the mechanism for a code mod
to replace guest *code* -- any recompiled game function, at any guest
address, can be swapped for a host-side implementation:

```cpp
::PPCFunc* g_original = nullptr;

void MyReplacement(PPCContext& ctx, uint8_t* base) {
  // ctx holds the guest register file (ctx.r3, ctx.r4, ... for args,
  // ctx.f1... for float args, base is the guest memory base for pointer
  // arithmetic). Call through to the original when you want to wrap rather
  // than fully replace:
  g_original(ctx, base);
}

void MyModPlugin::OnModuleLaunched() {
  auto* dispatcher = rex::runtime::Runtime::instance()->function_dispatcher();
  dispatcher->OverrideFunction(0x82012340, &MyReplacement, &g_original);
}

void MyModPlugin::OnShutdown() {
  auto* dispatcher = rex::runtime::Runtime::instance()->function_dispatcher();
  dispatcher->RestoreFunction(0x82012340, g_original);
}
```

`guest_address` is the function's original PowerPC entry point (the same
address you'd publish/look up via `ModRegistry::RegisterAddress` /
`FindAddress`); `replacement` is a host-side `PPCFunc` (`void(PPCContext&,
uint8_t*)`). Unlike `SetFunction` (used internally during a module's own
registration), `OverrideFunction` isn't restricted to that registration
window -- it can be called at any time, including well after startup, and is
safe to call concurrently with guest execution: a thread already inside the
old function body finishes running it, but the next call sees the
replacement.

A few things worth knowing:

- **Overrides are exclusive, not chainable.** Only one mod can override a
  given address at a time; a second `OverrideFunction` call on an
  already-overridden address fails (returns `false`, logs) rather than
  stacking. If two enabled mods need to touch the same function, one has to
  wrap and call through to the other's replacement -- there's no built-in
  override chain, so that has to be coordinated between the mods (e.g. via
  `ModRegistry`, and generally the higher-priority mod, per `enabled_mods`
  order, should be the one that owns the override).
- **`out_original` is the value you must restore with.** `OverrideFunction`
  hands back the function pointer that was active before the call (which may
  itself be the *default* recompiled function, or another mod's override if
  chained by convention above it). Hold onto it; `RestoreFunction` requires
  passing the exact same pointer back and fails on a mismatch (stale/wrong
  restore).
- **Always restore in `OnShutdown`.** An override left in place past its
  owning mod's lifetime points at code that may no longer be valid (e.g. if
  the mod's DLL is unloaded), and blocks any other mod from overriding that
  address afterward since the slot is still marked exclusive.
- **This replaces the whole function, not a callsite.** Both direct (`bl`)
  and indirect (`bctrl`/function-pointer) calls to `guest_address` see the
  replacement -- there's no way to override just one caller's view of a
  function.

## The mod manager overlay (F1) and the public mod catalog

`rex::ui::ModManagerDialog` (`src/ui/overlay/mod_manager_overlay.cpp`) is a
read/write UI over everything above:

- **"Installed" tab** (always present): lists every folder under the mods
  root in `mods.toml` order, with a checkbox to enable/disable, Up/Down
  buttons to reorder, an "Auto-sort" button (`ModState::AutoSort` -- a
  stable topological sort over `requires`/`load_after` edges among enabled
  mods, pinning disabled entries to their slot), and per-row `[error]`/
  `[warning]` badges from `ModState::Validate` (the same rules as
  `Runtime::ValidateModDependencies`, as structured data instead of only
  logged, plus a check that a code mod actually ships a binary for the
  running host's `platform` list). Every mutation writes `mods.toml`
  immediately.
- **Restart banner**: shown whenever the live `mods.toml` state differs from
  what `Runtime::ModStateAtStartup()` snapshotted at boot (membership, order,
  or enabled flags), with a "Restart Now" button
  (`rex::platform::process::Relaunch()` + `Window::RequestClose()`).
- **"All" tab**: only rendered once `rex::system::ModCatalog` reaches
  `kReady` -- browses a public mod catalog over HTTPS (Firestore REST, no
  auth) and can install/update a mod in place. Entirely optional and
  game-agnostic:
  - `RuntimeConfig::catalog_name` (set by the downstream project in
    `OnPreSetup()`) is the catalog identity string the query filters on.
    Empty disables the tab outright.
  - The endpoint itself is fully cvar-driven (`mod_catalog_project`,
    `mod_catalog_api_key`, `mod_catalog_url` -- a full override that wins
    over the first two, for a self-hosted/non-Firestore backend serving the
    same wire shape -- `mod_catalog_games_collection`,
    `mod_catalog_mods_collection`). Clearing `catalog_name` or both
    `mod_catalog_project`/`mod_catalog_url` disables the catalog exactly like
    a network failure would: the tab is simply omitted, nothing throws, and
    the "Installed" tab keeps working.
  - Any failure -- DNS, TLS, HTTP error, malformed JSON -- leaves
    `ModCatalog` in a `kFailed` state with the same "no tab, no crash"
    behavior.
  - Installing verifies the downloaded asset's SHA-256 against the
    catalog's `checksum` and **hard-refuses on mismatch**, never
    extracting a payload that doesn't match what was approved.
- **Sideloading (drag-and-drop)**: dropping a `.zip` onto the game window
  (`ReXApp::OnFileDrop`, `src/ui/rex_app.cpp`) installs it the same way a
  catalog install does -- extract, unwrap a single top-level directory if
  present (otherwise treat the archive as flat and derive the id from the
  zip's own file stem), rename into `<mods_root>/<id>` -- via
  `ModState::InstallLocalArchive`. Unlike a catalog install (which trusts the
  catalog's `modId`), a dropped zip has no other signal to confirm it's
  actually a mod, so the resolved content root **must contain a `mod.toml`**
  or the install is refused; anything else dropped onto the window (a
  non-`.zip` file, or a `.zip` with no manifest) is silently ignored/rejected
  rather than treated as a mod. If a mod of the same id is already installed,
  the drop only replaces it when its `mod.toml` `version` is >= the installed
  one's (same not-older-clobbers-newer rule as a catalog update); otherwise
  it's refused. This opens (or reuses) the mod manager overlay automatically
  and scrolls/highlights the (re)installed mod's row in the "Installed" tab,
  so the result is visible without the player having to press F1 themselves.
  A mod installed this way carries no persisted "how did this get here"
  flag -- the overlay marks any installed mod not present in the currently
  loaded catalog snapshot as `[Sideloaded]`, purely as a derived comparison
  against `ModCatalog::Snapshot()` (so it only shows once the "All" tab has
  data; nothing is marked when the catalog is disabled/unreachable).

## `platform` strings and per-platform `code/` subdirectories (code mods only)

The SDK's own `ModInfo`/`ParseModInfo` now parses a `platform` key into
`ModInfo::platforms`, and it **is enforced**, at two different points:

- `ModState::Validate` (used by the mod manager overlay's "Installed" tab,
  and by `ModCatalog`'s "All" tab compatibility check) treats a code mod
  (one with a `code` key) that declares no `platform` entries, or whose
  `platform` list doesn't include the running host's platform id
  (`windows-x64`/`linux-x64`/`linux-arm64`), as a **hard error** -- same
  severity as an unmet `requires` version constraint -- surfaced as a
  per-row `[error]` badge the player has to act on (update, disable, or
  remove the mod). This is the primary enforcement point, since it runs
  every time the overlay is open, not just at boot.
- `Runtime::ValidateModDependencies()` (the boot-time check in
  `runtime.cpp`, which only fails `Setup()` on a version mismatch) does
  *not* itself gate on `platform` -- a mod with no binary for the host still
  reaches `LoadModPlugin`, which resolves `code/<platform>/<stem>.dll`
  (falling back to the flat `code/<stem>.dll`) and simply fails to find a
  library file, logging an error and skipping that mod's code plugin rather
  than blocking the whole game from starting. In practice a mismatched
  `platform` is caught well before that: `ModState::Validate` flags it as
  soon as the mod manager overlay is opened, well before `Setup()` runs.

It remains a convention for a downstream project's *own* mod-build tooling
to populate correctly -- the SDK doesn't build or stamp `platform` itself,
only reads and enforces what's already there.

The convention: a `platform` key in a code mod's `mod.toml`, holding a
comma-separated list of target identifiers (e.g.
`"windows-x64,linux-x64,linux-arm64"`) recording which platform(s) that
mod's `code/` directory currently ships a binary for. It is written by the
build tooling after a build, not by the mod author, and reflects what's
actually on disk right now, not a request or a restriction to build for
that platform.

`LoadModPlugin` (`src/system/mod_plugin_loader.cpp`) resolves those same
target identifiers as an optional subdirectory under `code/`: it first looks
for `code/<platform>/<stem>.dll` (or `lib<stem>.so`), where `<platform>` is
whichever one of `windows-x64`, `linux-x64`, `linux-arm64` matches the
running host (`REX_PLATFORM_*`/`REX_ARCH_*` at compile time), and falls back
to the flat `code/<stem>.dll`/`code/lib<stem>.so` if no matching
subdirectory exists. This is what lets a single mod folder -- and therefore
a single distributed archive -- carry binaries for every platform side by
side: a flat `code/` can hold at most one Linux `.so` (linux-x64 and
linux-arm64 both build to the same `lib<stem>.so` name and would collide),
so multi-platform distributions need the subdirectory form for at least the
two Linux targets. A locally-built, single-platform mod can still use the
flat layout; both are checked.

Asset-only mods (no `code` key) have nothing to record: they ship no native
binary, so there is no per-platform artifact to track, and none of this
applies to them at all.
