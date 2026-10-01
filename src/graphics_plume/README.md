# Plume renderer v3 changes

Applied on top of the previously validated v2 renderer changes.

## Changes

### Command processor
- Restored two-frame synchronization from the upstream renderer:
  - per-frame acquire semaphore
  - per-frame render semaphore
  - per-frame fence/in-flight state
- `IssueDraw` waits for the current frame fence before reusing/resetting its command list.
- `IssueSwap`:
  - acquires the swapchain image with the acquire semaphore
  - ends the active framebuffer/render pass before image barriers
  - selects the primary guest color target using frontbuffer dimensions
  - copies guest color target -> swapchain
  - signals a render semaphore
  - presents waiting on that render semaphore
- Restored EDRAM copy-mode dispatch from `IssueDraw` to `IssueCopy`.
- Added `IssueDraw ENTER/EXIT` and `IssueSwap ENTER/EXIT` diagnostics using REX logging, so the next run can show whether the guest reaches draw/present execution.
- Preserved the known-good 256 MiB `GPU_UPLOAD + STORAGE` shared-memory allocation.
- Preserved conditional depth-target creation.
- Desktop 8:8:8:8 render targets use `B8G8R8A8_UNORM`.

### Render-target cache
- Restored primary framebuffer tracking.
- Primary presentation selection prefers a framebuffer matching the guest frontbuffer height and sufficient width, then falls back to the largest widescreen framebuffer.
- Depth textures are only created when a depth format is actually requested.
- Desktop color targets use BGRA.

### Graphics system
- Preserved SDL3 -> X11 native-window bridge used successfully on Linux.
- Preserved retry behavior when presentation setup happens before the SDL window exists.
- Swapchain format is platform-specific:
  - Android: `R8G8B8A8_UNORM`
  - Windows/Linux desktop: `B8G8R8A8_UNORM`

## Test environment
The v2 initialization path was already validated on Intel Iris Xe with:

```bash
SDL_VIDEODRIVER=x11 ./your-game
```

The next run should specifically reveal whether `IssueDraw` and `IssueSwap` are reached after `SetupContext complete`.
