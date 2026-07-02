/**
 * @file        rex/graphics/pipeline/texture/replacement.h
 *
 * @brief       Texture dump and replacement pipeline.
 *
 *              Textures are identified by a stable content hash (XXH3 over the
 *              raw guest bytes as seen in physical memory — tiled and
 *              big-endian, exactly as the Xbox GPU would read them).  This
 *              makes the hash address-independent and run-independent.
 *
 *              Dump layout (under the host dump root passed to the constructor):
 *                  textures/<hash16>_<w>x<h>_<fmt_name>.dds   (default)
 *                  textures/<hash16>_<w>x<h>_<fmt_name>.png   (when texture_dump_format="png")
 *
 *              Replacement layout: replacements are looked up across an ordered
 *              list of mod roots (Runtime::ModOverlayRoots("textures")), one
 *              per enabled mod that has a textures/ subfolder, highest-priority
 *              mod first. The first root providing a given content hash wins;
 *              Rescan() re-walks all roots and rebuilds the index.
 *                  <mod_root>/<hash16>.dds   (any power-of-2 resolution)
 *                  <mod_root>/<hash16>.png   (RGBA8; loaded via stb_image)
 *
 *              Dump DDS format:
 *                - Compressed formats (DXT1/DXT3/DXT5/DXN/CTX1/DXT3A/DXT5A):
 *                    FOURCC DDS with raw BC blocks — lossless, directly usable
 *                    as a base for replacement authoring.
 *                - Uncompressed formats: RGBA8 unorm after untiling + endian
 *                    swap + channel expansion to 8bpc.
 *
 *              Dump PNG format (texture_dump_format="png"):
 *                - All formats expanded to RGBA8 unorm PNG.
 *                - BC-compressed formats are software-decompressed to RGBA8
 *                  before encoding (DXT1/3/5, BC4/ATI1, BC5/ATI2 supported).
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

// CVARs controlling the dump/replace pipeline (defined in cache.cpp).
REXCVAR_DECLARE(bool, texture_dump_enabled);
REXCVAR_DECLARE(std::string, texture_dump_format);
REXCVAR_DECLARE(std::string, texture_dump_skip_sizes);

namespace rex::graphics {

// ---------------------------------------------------------------------------
// Replacement descriptor returned to the injection path
// ---------------------------------------------------------------------------
struct TextureReplacementData {
  // Decoded pixels: RGBA8 unorm, row-major, tightly packed (no row padding).
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  // Number of mip levels present in the replacement file (>= 1).
  uint32_t mip_levels = 1;
};

// ---------------------------------------------------------------------------
// TextureReplacement
// ---------------------------------------------------------------------------
class TextureReplacement {
 public:
  // mod_roots is ordered by mod priority (first = highest); the first root
  // providing a given content hash wins. dump_root is the host directory
  // dumped textures are written under (as dump_root/textures).
  TextureReplacement(std::vector<std::filesystem::path> mod_roots, std::filesystem::path dump_root);
  ~TextureReplacement() = default;

  TextureReplacement(const TextureReplacement&) = delete;
  TextureReplacement& operator=(const TextureReplacement&) = delete;

  // Rescans all mod roots and rebuilds the hash→path index. Earlier
  // (higher-priority) roots win over later ones for the same hash.
  void Rescan();

  // ---------------------------------------------------------------------------
  // Dump path
  // ---------------------------------------------------------------------------
  // Untiles, endian-swaps, and writes the guest texture to a DDS file.
  //   guest_bytes  : raw physical memory at base_page << 12
  //   guest_size   : GetGuestBaseSize() bytes
  //   width/height : texel dimensions
  //   pitch_blocks : TextureKey::pitch (pitch in units of 32 texels)
  //   tiled        : TextureKey::tiled
  //   format       : TextureKey::format
  //   endianness   : TextureKey::endianness
  void DumpTexture(uint64_t content_hash, uint32_t width, uint32_t height, uint32_t pitch_blocks,
                   bool tiled, xenos::TextureFormat format, xenos::Endian endianness,
                   const uint8_t* guest_bytes, uint32_t guest_size) const;

  // ---------------------------------------------------------------------------
  // Injection path
  // ---------------------------------------------------------------------------
  // Returns a pointer into the internal cache, or nullptr if not found.
  // The pointer is valid until the next call to Rescan().
  [[nodiscard]] const TextureReplacementData* FindReplacement(uint64_t content_hash) const;

  // ---------------------------------------------------------------------------
  // Hash
  // ---------------------------------------------------------------------------
  static uint64_t HashGuestData(const uint8_t* data, size_t size);

  std::filesystem::path dump_dir() const { return dump_dir_; }
  const std::vector<std::filesystem::path>& mod_roots() const { return mod_roots_; }

 private:
  std::vector<std::filesystem::path> mod_roots_;
  std::filesystem::path dump_dir_;
  std::unordered_map<uint64_t, std::filesystem::path> replacements_;

  // Textures that have been loaded from disk are cached here so that
  // FindReplacement never touches the filesystem after the first load.
  mutable std::unordered_map<uint64_t, TextureReplacementData> pixel_cache_;
  // Hashes that failed to load are remembered so we don't retry every frame.
  mutable std::unordered_set<uint64_t> failed_cache_;

  static bool WriteDDS_RGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                             const uint8_t* rgba8_rows, uint32_t row_pitch_bytes);

  static bool WriteDDS_BC(const std::filesystem::path& path, uint32_t width, uint32_t height,
                          const uint8_t* bc_blocks, uint32_t bytes_per_block, uint32_t fourcc);

  static bool WritePNG_RGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                             const uint8_t* rgba8_rows, uint32_t row_pitch_bytes);

  static bool ReadDDS(const std::filesystem::path& path, TextureReplacementData& out);

  static bool ReadPNG(const std::filesystem::path& path, TextureReplacementData& out);
};

}  // namespace rex::graphics
