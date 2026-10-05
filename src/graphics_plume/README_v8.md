# Plume renderer v8 changes

This patch is deliberately conservative. It does **not** pretend that marking a
resolve range written is equivalent to executing the Xenos EDRAM resolve.

Changes:
- `CreateRenderTarget` now uses the guest render-target height instead of a hardcoded 720.
- `GetCurrentFramebuffer` is implemented.
- `MVP_GetPrimaryColorTexture` now prefers an exact frontbuffer-size framebuffer,
  then the smallest framebuffer containing the requested dimensions. This avoids
  unordered-map selection of a larger scene/shadow target.
- `Resolve` logs the actual Xenos resolve rectangle and destination range.

Important limitation:
- The Plume `RenderInterface` headers/submodule are not included in the supplied
  source archive, so a true EDRAM->guest-memory compute resolve cannot safely be
  authored/compiled against the exact Plume API from this archive alone.
- The existing `Resolve()` still does not emit a GPU resolve. v8 therefore focuses
  on selecting the correct presentation surface and making the missing resolve
  observable rather than introducing an unverified API dependency.
