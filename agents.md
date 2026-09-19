# ReXGlue SDK — Agent Notes

Internal runbook for building the SDK and running recompiled Xbox 360 titles
with the **native renderer** (`rexgpu-native`). Facts here were verified against
the `nativeRenderer` branch (commit `18943e8`).

---

## 1. Branch map

| Branch | Purpose | Renderer state |
| --- | --- | --- |
| `main` | Older baseline | No native renderer (`src/graphics/native/` absent) |
| `native` | SDK release branch (v0.10.0 floor) | No native renderer |
| `sotn` | Symphony-of-the-Night-style port work | **No** native renderer — only the emulated `rexgpu-xenos` backend |
| `nativeRenderer` (current) | Native renderer bring-up | Ships `rexgpu-native` plugin (game-agnostic Vulkan backend) |

- `merge-base(sotn, nativeRenderer)` = `29eaa8a`, so they diverged long ago.
- `sotn` tip (`7766f97`) is a `feat/fix` stream (mods, achievements, UI, audio,
  D3D12, leaderboards). `nativeRenderer` tip (`18943e8`) is the native-renderer
  stream (scaffold through final fixes).
- `upstream/development` also contains `src/graphics/native/` — the native
  renderer is not just on this work branch.

---

## 2. What the native renderer actually is

The native renderer is a second, **game-agnostic** GPU plugin that replaces the
emulated Xenos backend (`rexgpu-xenos`) without touching any game code.

Everything lives under `src/graphics/native/`:

```
src/graphics/native/
  plugin_main.cpp          # extern "C" rex_gpu_create / rex_gpu_abi_version entry points
  graphics_system.{h,cpp}  # NativeGraphicsSystem : GraphicsSystem
  command_processor.{h,cpp} # NativeCommandProcessor — the whole backend (PM4 -> Vulkan)
  native_shared_memory.{h,cpp} # 512 MB host-visible guest-RAM mirror (one big VkBuffer)
  texture_cache.{h,cpp}     # NativeTextureCache — real guest texture decode on GPU
  draw_classify.h           # pure decision table: what a draw becomes
  index_expand.h            # quad-list / rectangle-list -> triangle-list expansion
  phase_model.h             # EDRAM "phase" ownership model (no EDRAM on host)
  shader_constants.h        # register-derived clip/vertex state
```

Key design points (from the source comments and `graphics_system.h`):

- `NativeCommandProcessor` derives from the shared, game-agnostic
  `rex::graphics::CommandProcessor`. That base class does **all** PM4 ring-buffer
  parsing and hands decoded work through virtual seams (`SetupContext`,
  `LoadShader`, `IssueDraw`, `IssueCopy`, `IssueSwap`).
- Only the backend draw/resolve/present seams are overridden. The emulated
  backend (`vulkan/command_processor.cpp`, `vulkan/texture_cache.cpp` …) keeps
  its own EDRAM/tiling/resolve emulation; the native backend has **no EDRAM**.
- Shaders: guest Xenos microcode is translated to SPIR-V by reusing the shared
  translator (`spirv_translator_*`), and geometry is fetched in-shader from the
  512 MB shared-memory buffer (no classic vertex input).
- Frames are built as a **deferred draw list** replayed at `IssueSwap`; guest
  render-target "phases" are marked by `IssueCopy` resolves and selected by the
  pure-arithmetic model in `phase_model.h` / `draw_classify.h` (unit-tested in
  `tests/unit/graphics/`).

The three "bring-up" headers are deliberately free of Vulkan/registers so they
can be unit-tested without a GPU or running title:

- `tests/unit/graphics/draw_classify_test.cpp`
- `tests/unit/graphics/index_expand_test.cpp`
- `tests/unit/graphics/phase_model_test.cpp`
- `tests/unit/graphics/shader_constants_test.cpp`

### CMake wiring (`src/graphics/CMakeLists.txt`, lines ~147–245)

- `rexgpu-native` is a `SHARED` library, built **only when
  `REXGLUE_USE_VULKAN=ON`** (the default on Linux; Vulkan-only for now).
- It recompiles the shared PM4/Xenos base sources plus the SPIR-V translator and
  `vulkan/geometry_shader_builder.cpp`, so it is fully independent of
  `rexgpu-xenos` (it neither modifies nor links it).
- It links `rexruntime` (PRIVATE) plus glslang/volk/VMA/xxHash/snappy.

---

## 3. Building the SDK (Linux)

Requirements (verified against `CMakeLists.txt` / `CMakePresets.json`):

- **Clang ≥ 18** (hard requirement — the build fatal-errors otherwise)
- **CMake ≥ 3.27** (this branch bumped the minimum from 3.25)
- **Ninja** (Multi-Config generator)
- **Vulkan headers/loader**, C++23

```sh
cd /home/droidsavior/Documents/rexglue-sdk-fork
git submodule update --init --recursive      # thirdparty/* are submodules

cmake --preset linux-amd64                    # Ninja Multi-Config, Clang
cmake --build --preset linux-amd64-release    # Release config
```

Build artifacts land in `out/linux-amd64/Release/`:

```
out/linux-amd64/Release/
  rexglue                      # CLI (codegen/init)
  librexruntime.so             # runtime shared lib
  librexgpu-xenos.so           # emulated GPU plugin (the default/fallback)
  librexgpu-native.so          # <-- the native renderer plugin
```

> Plugin naming is config-driven (see `src/system/gpu_plugin_loader.cpp`):
> Linux `librexgpu-<name><postfix>.so`, where postfix is `""` (Release),
> `d` (Debug), `rd` (RelWithDebInfo). So the native plugin is
> `librexgpu-native.so` / `librexgpu-natived.so` / `librexgpu-nativerd.so`.
> Windows drops the `lib` prefix: `rexgpu-native.dll`.

To also build/run the native-renderer unit tests:

```sh
cmake --preset linux-amd64 -DREXGLUE_BUILD_TESTS=ON
cmake --build --preset linux-amd64-release
ctest --preset linux-amd64-release            # runs unit.* incl. graphics/*
```

(`REXGLUE_BUILD_TESTS` is OFF by default; the four native `graphics/*` tests are
registered in `tests/unit/CMakeLists.txt`.)

---

## 4. Running a recompiled game with the native renderer

### How the plugin is selected and loaded

1. The app's `ReXApp::SetupPresentation()` reads the `gpu_plugin` cvar
   (`src/ui/rex_app.cpp:49,334`) and calls `rex::system::LoadGpuPlugin(name)`.
2. `LoadGpuPlugin` (`src/system/gpu_plugin_loader.cpp`) computes
   `GetExecutableFolder() / "librexgpu-native.so"` and `dlopen`s **that absolute
   path** — it does *not* search `LD_LIBRARY_PATH` for the plugin itself.
3. Plugin dependencies (`librexruntime.so`, third-party libs) are found via the
   host exe's `$ORIGIN` RPATH (set by `rexglue_configure_target`) or
   `LD_LIBRARY_PATH`.

So there are **two** things that must be right:

- The **plugin `.so` must be next to the game executable**.
- The **runtime libs must be resolvable** (RPATH `$ORIGIN` handles this when
  `librexruntime.so` is staged alongside the exe, which the SDK does).

### Step-by-step

1. Build the SDK (section 3) so `librexgpu-native.so` exists.

2. Make sure the recompiled project stages the native plugin next to its
   host executable. In the generated `CMakeLists.txt` (inja template
   `resources/templates/init/rexglue_cmake.inja`), change:

   ```cmake
   rexglue_setup_target(mygame)                        # before
   rexglue_setup_target(mygame GPU_PLUGINS native)     # after
   ```

   `rexglue_setup_target` forwards `GPU_PLUGINS` to `rexglue_configure_target`
   (`cmake/rexglue_helpers.cmake:105`), which `copy_if_different`s
   `librexgpu-native.so` next to the exe on every build.

   > Caveat: only `rexgpu-xenos` is in `REXGLUE_INSTALL_TARGETS`
   > (`cmake/rexglue_install.cmake:14`). `rexgpu-native` is **not** installed
   > into an installed-SDK package, so `GPU_PLUGINS native` works with the
   > in-tree SDK (`add_subdirectory` / `REXSDK_DIR` flow), not a
   > `find_package(rexglue)` install. For an installed SDK, copy
   > `librexgpu-native.so` out of the SDK build tree next to the game exe by hand.

3. Run the game with the plugin selected — set `gpu_plugin` on the command line,
   via env, or in the game's config (section 5):

   ```sh
   ./mygame --gpu_plugin native --game_data_root /path/to/game/assets
   ```

   A green tab is drawn in the top-left corner of every native frame by default
   (`native_marker=1`), so confirm at a glance the native renderer (and not
   `rexgpu-xenos`) is active.

---

## 5. Turning native renderer knobs

All ReXGlue cvars can be set three ways (last-in wins per the startup order in
`src/core/cvar.cpp` `cvar::Init` / `ApplyEnvironment` / `LoadConfig` /
`RegisterFlag`):

1. **Command line**: `--name value` (bool: `--name` / `--no-name`; also
   `--name=value`).
2. **Environment**: `REX_<NAME>` uppercased, e.g. `REX_GPU_PLUGIN=native`.
3. **Config file** `<app-name>.toml` next to the exe (loaded in
   `ReXApp::OnInitialize`; `ApplyTomlTable` maps nested tables to
   `prefix_key`).

The config file uses the cvar name as the key (flat or nested under a category):

```toml
gpu_plugin = "native"
[GPU]
native_log_draws = true
```

### Core cvars for the native path

| Cvar | Default | Meaning |
| --- | --- | --- |
| `gpu_plugin` | `""` (disabled) | Plugin to load at startup. `native` → `librexgpu-native.so`; `xenos` → emulated backend. `kInitOnly` (restart required). |
| `video_mode_width` / `video_mode_height` | — | Native branch removed the 1920×1080 ceiling; presets are used verbatim (`xboxkrnl_video.cpp`). |
| `frame_limit` | `0` (uncapped) | Caps guest `VdSwap` presents/sec for titles whose present loop free-runs. Added on the native branch. |

### Native-renderer-specific cvars (`src/graphics/native/command_processor.cpp`)

| Cvar | Default | Purpose |
| --- | --- | --- |
| `native_marker` | `true` | Green corner tab on every native frame. |
| `native_marker_empty_frames` | `false` | Flash draw-less frames mid-blue (bring-up aid). |
| `native_log_draws` | `false` | Log every draw/copy the PM4 stream delivers (flood). |
| `native_log_phases` | `false` | Log render-target phases + resolves (cheap, periodic). |
| `native_expand_rects` | `false` | Expand rectangle lists to triangles on the CPU path. Off: corner reconstructed in the vertex shader (not yet proven). |
| `native_rect_gs` | `true` | Use the oracle's validated geometry-shader rectangle expansion. |
| `native_phase_base_filter` | `true` | Restrict phase replay to draws whose RT matches the resolved EDRAM base. |
| `native_present_frontbuffer` | `false` | Present the resolved front-buffer image instead of replaying draws (selection is correct; blit still fails vkQueueSubmit, so off). |

### RenderDoc forensics

IssueSwap carries a headless RenderDoc hook (`command_processor.cpp` around line
3278), no overlay/keyboard needed:

```sh
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 \
REX_RENDERDOC_CAPTURE_FRAME=<n>            # capture at guest swap <n>
# or
REX_RENDERDOC_CAPTURE_DRAWS=<min>:<k>      # capture at the k-th swap issuing >= <min> draws
REX_RENDERDOC_CAPTURE_PATH=/tmp/cap.rdc    # optional path template
```

---

## 6. Troubleshooting checklist (black world / missing geometry)

Ordered by what the source comments say is most likely:

1. **Wrong plugin loaded** — no green tab = you're on `rexgpu-xenos`, not the
   native renderer. Confirm `--gpu_plugin native` actually resolved.
2. **Plugin not found** — log shows `GPU plugin 'native' not found at
   <exe>/librexgpu-native.so`. Stage it (`GPU_PLUGINS native`) or copy it next
   to the exe. Note this load is from the **exe folder only**, not
   `LD_LIBRARY_PATH`.
3. **Reverse-Z titles render a black world but correct HUD** — depth was once
   cleared to a hardcoded 1.0; `guest_depth_clear_` now reads `RB_DEPTH_CLEAR`
   (`IssueSwap`). Still check `native_log_phases` output.
4. **Draws dropped** — run with `native_log_draws=1`; `ShutdownContext` prints a
   cumulative `skipped=` histogram. `draw_classify.h` documents the exact
   ordering that once mis-classified a depth pre-pass as "skip" (black 3D in
   Hydro Thunder).
5. **HDR titles blow out white** — guest renders into a float scene image
   (`R16G16B16A16_SFLOAT`), not 8-bit UNORM; if you see clipping, investigate
   the scene-image → swap blit rather than the draw path.
6. **Missing rect half (diagonal seam)** — `native_rect_gs` / `native_expand_rects`
   are A/B switches; rectangle lists supply only 3 corners (4th implied).

---

## 7. Notes / gotchas for an agent

- `gpu_plugin` is `kInitOnly`; changing it needs a process restart.
- The plugin ABI is versioned (`kGpuPluginAbiVersion = 1`, exports
  `rex_gpu_create` / `rex_gpu_abi_version` in `include/rex/system/gpu_plugin.h`).
  Bump the version when `GpuCreateInfo` or `IGraphicsSystem` changes.
- `rexgpu-native` is off on Windows by default (Vulkan is `OFF` there; D3D12 is
  the Windows backend, and there is no native-D3D12 target).
- Do not add `rexgpu-native` to `REXGLUE_INSTALL_TARGETS` casually — that list is
  also the `find_package(rexglue)` export set; if you do, consumers gain a
  `rex::gpu-native` imported target and `GPU_PLUGINS native` starts working with
  installed SDKs too.
- The green "native marker" and the `native_marker=false` absence check are your
  fastest "am I on the native renderer?" signals during bring-up.
