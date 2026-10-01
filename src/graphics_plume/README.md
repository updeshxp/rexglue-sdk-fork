# Plume renderer v5

Based on the Windows-successful v4 renderer plus the relevant Xerenge Plume upload/lifecycle approach.

## Key fix

The Linux log showed the runtime stall at:

    PlumeTextureCache: initializing PlumeTextureCache/uploader

The old `PlumeTextureUploader::Initialize()` created a compute pipeline layout even though the current upload implementation does CPU untile + `copyTextureRegion` and never uses those compute pipelines. Xerenge's Plume draw path likewise uses host staging/copy resources rather than this unused startup compute layout.

v5 therefore:

- keeps `PlumeTextureCache` deferred until runtime;
- makes `PlumeTextureCache::Initialize()` a lightweight no-op;
- initializes the texture uploader on the first actual texture upload;
- removes the unused compute pipeline-layout creation from uploader initialization;
- creates/maps only the 16 MiB staging buffer during uploader initialization;
- preserves all v4 changes: GPU_UPLOAD+STORAGE shared memory, X11 swapchain, BGRA desktop/RGBA Android, frame semaphores/fences, primary framebuffer selection, EDRAM copy path, and draw/swap diagnostics.

## Files

Replace these files in `graphics_plume/`:

- plume_command_processor.cpp
- plume_command_processor.h
- plume_graphics_system.cpp
- plume_graphics_system.h
- plume_render_target_cache.cpp
- plume_render_target_cache.h
- plume_texture_cache.cpp
- plume_texture_cache.h
- plume_texture_uploader.cpp
- plume_texture_uploader.h


## v6 changes

- Runtime texture/render-target cache objects are initialized from `PlumeCommandProcessor::Initialize()` on the caller/main graphics-system thread instead of lazily from `IssueSwap()` on the GPU worker thread.
- `PlumeTextureUploader` remains lazy and is initialized only when the first resident texture upload is actually required.
- Added fine-grained uploader logs around staging-buffer creation and mapping so the next runtime test can identify an upload-resource stall precisely.
- `EnsureRuntimeCaches()` is now a validation guard only; it no longer performs potentially blocking cache construction on the command-processor worker thread.
