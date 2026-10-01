# Plume renderer v4

Based on the Windows-successful v3 renderer, with the relevant lifecycle pattern from the Xerenge Plume implementation imported.

## Main change

PlumeTextureCache and PlumeRenderTargetCache initialization is now deferred until the command processor actually reaches IssueDraw/IssueSwap/IssueCopy.

The base ReXGlue CommandProcessor starts its GPU worker thread from Initialize(), and SetupContext runs on that worker thread. Keeping the relatively heavyweight Plume texture uploader/pipeline/staging initialization out of the startup path prevents a stall immediately after `SetupContext complete`.

This follows the useful architectural separation visible in Xerenge: presentation/device setup is established first, while draw-side resources are initialized when the guest GPU path needs them.

## Preserved from v3

- 256 MiB shared memory buffer using GPU_UPLOAD + STORAGE
- X11 SDL swapchain support
- Desktop BGRA / Android RGBA swapchain format split
- Conditional depth target handling
- Primary framebuffer selection
- Acquire/render semaphores
- Fence-safe frame reuse
- `setFramebuffer(nullptr)` before copy barriers
- EDRAM copy-mode dispatch
- Windows linker fix for `plume_swapchain()`
- IssueDraw / IssueSwap diagnostic logging

## Deliberately not imported from Xerenge

Xerenge has a separate direct Plume draw/MMIO architecture. Its direct GPU MMIO mapping, ring consumer, draw context, shader cache and complete presentation worker are not compatible with this rexglue CommandProcessor backend, so they are not copied into this patch.

## Expected next Linux log

The first important new line should be:

    PlumeCommandProcessor: base Initialize returned; deferring texture/render-target caches

If the guest reaches IssueSwap first, the log should then show:

    PlumeCommandProcessor: initializing deferred Plume runtime caches
    PlumeCommandProcessor: creating PlumeTextureCache
    PlumeCommandProcessor: initializing PlumeTextureCache/uploader

This will both remove the startup dependency and pinpoint any remaining Plume uploader/resource issue.
