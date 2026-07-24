/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstring>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::graphics {

class GraphicsSystem;
class Shader;

enum class ReadbackResolveMode {
  kDisabled,
  kFast,
  kSome,
  kFull,
};

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  // Dimensions of the framebuffer textures. Should match window size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Current front buffer, being drawn to the screen.
  uintptr_t front_buffer_texture = 0;
  // Current back buffer, being updated by the CP.
  uintptr_t back_buffer_texture = 0;
  // Backend data
  void* backend_data = nullptr;
  // Whether the back buffer is dirty and a swap is pending.
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

class CommandProcessor {
 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  virtual ~CommandProcessor();

  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }

  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }

  // Snapshot of a single tracked shader for the debugger UI. Plain data so
  // callers don't need to touch graphics-internal Shader objects from other
  // threads.
  struct ShaderInfo {
    uint64_t ucode_hash = 0;
    xenos::ShaderType type = xenos::ShaderType::kVertex;
    uint32_t dword_count = 0;
    bool disabled = false;
    bool active = false;
    // Per-shader profiling data. Only meaningful while
    // IsShaderProfilingEnabled() is true; otherwise the counters are stale or
    // zero. profile_total_ns is summed CPU time inside IssueDraw across all
    // draws this shader participated in since the last reset.
    uint64_t profile_total_ns = 0;
    uint64_t profile_draw_count = 0;
  };

  // Per-translation snapshot: there is one of these for every (shader,
  // modification bits) host pipeline variant the backend has produced.
  struct ShaderTranslationInfo {
    uint64_t modification = 0;
    bool is_translated = false;
    bool is_valid = false;
    // Host backend-specific disassembly text (e.g. DXBC ASM, SPIR-V text).
    std::string host_disassembly;
    // Raw translated binary bytes (DXBC/DXIL/SPIR-V/...).
    std::vector<uint8_t> translated_binary;
  };

  // Full detail for a single shader, used by the debugger viewer pane.
  struct ShaderDetails {
    bool found = false;
    ShaderInfo info;
    // Xenos microcode disassembly (D3D format). Always available once the
    // shader's ucode has been analyzed (which the debugger forces).
    std::string ucode_disassembly;
    // Raw microcode dwords (host endianness).
    std::vector<uint32_t> ucode_dwords;
    std::vector<ShaderTranslationInfo> translations;
  };

  // Returns a thread-safe snapshot of every shader currently tracked by the
  // backend's pipeline cache. Default returns an empty list -- backends that
  // maintain a shader cache should override.
  virtual std::vector<ShaderInfo> GetShaderSnapshot() const { return {}; }

  // Toggle the disabled flag on a previously enumerated shader. Looks the
  // shader up by its ucode hash. No-op if the hash is unknown. Default does
  // nothing -- backends override.
  virtual void SetShaderDisabledByHash(uint64_t ucode_hash, bool disabled) {
    (void)ucode_hash;
    (void)disabled;
  }

  // Returns full details for a shader (ucode disassembly + every translation).
  // Returns ShaderDetails{found=false} if the hash is unknown. Default does
  // nothing -- backends override.
  virtual ShaderDetails GetShaderDetails(uint64_t ucode_hash) const {
    (void)ucode_hash;
    return {};
  }

  // Replaces the translated host binary for one (shader, modification) pair
  // and invalidates any cached pipeline state objects that referenced the old
  // binary so the GPU picks up the new code on the next draw. Returns true if
  // the shader was found and the replacement was queued.
  virtual bool ReplaceShaderTranslationBinary(uint64_t ucode_hash, uint64_t modification,
                                              std::vector<uint8_t> binary) {
    (void)ucode_hash;
    (void)modification;
    (void)binary;
    return false;
  }

  // Compiles HLSL source on the host, then forwards the resulting bytecode to
  // ReplaceShaderTranslationBinary for one (shader, modification) pair. The
  // backend chooses the compiler (e.g. D3DCompile for D3D12). Pass an empty
  // `entry_point` to use the backend default ("main") and an empty
  // `target_profile` to let the backend infer it from the shader type
  // (vs_5_1 / ps_5_1 for D3D12). Returns true if compilation and replacement
  // both succeeded. Default does nothing -- backends override.
  virtual bool ReplaceShaderTranslationHLSL(uint64_t ucode_hash, uint64_t modification,
                                            std::string_view source,
                                            std::string_view entry_point = {},
                                            std::string_view target_profile = {},
                                            std::string* out_error = nullptr) {
    (void)ucode_hash;
    (void)modification;
    (void)source;
    (void)entry_point;
    (void)target_profile;
    if (out_error) {
      *out_error = "HLSL replacement not supported by this backend.";
    }
    return false;
  }

  // Convenience: compile HLSL once and apply it to *every* translation
  // (modification permutation) currently associated with `ucode_hash`. This is
  // what most callers want when they don't care about modification keys -- the
  // common "swap this shader by hash" workflow. Walks GetShaderDetails() to
  // enumerate modifications and forwards each one through
  // ReplaceShaderTranslationHLSL. Returns true if at least one translation was
  // successfully replaced; sets *out_replaced_count if non-null.
  //
  // Defined inline (not in command_processor.cpp): this class's out-of-line
  // methods are compiled only into the optional GPU plugin (rexgpu-xenos),
  // never linked into the consumer executable. Callers outside the plugin
  // (e.g. rex_app.cpp, which links against rexruntime only) can still reach
  // virtual methods through vtable dispatch, but a direct, non-virtual call
  // needs the body available at the call site.
  bool ReplaceShaderHLSL(uint64_t ucode_hash, std::string_view source,
                         std::string_view entry_point = {}, std::string_view target_profile = {},
                         std::string* out_error = nullptr, size_t* out_replaced_count = nullptr) {
    if (out_replaced_count) {
      *out_replaced_count = 0;
    }
    ShaderDetails details = GetShaderDetails(ucode_hash);
    if (!details.found) {
      if (out_error) {
        *out_error = "Shader hash not loaded by the GPU yet.";
      }
      return false;
    }
    if (details.translations.empty()) {
      if (out_error) {
        *out_error = "Shader has no translations to replace.";
      }
      return false;
    }
    size_t replaced = 0;
    std::string per_call_error;
    std::string last_error;
    for (const auto& tr : details.translations) {
      if (ReplaceShaderTranslationHLSL(ucode_hash, tr.modification, source, entry_point,
                                       target_profile, &per_call_error)) {
        ++replaced;
      } else if (!per_call_error.empty()) {
        last_error = per_call_error;
      }
    }
    if (out_replaced_count) {
      *out_replaced_count = replaced;
    }
    if (replaced == 0 && out_error) {
      *out_error =
          last_error.empty() ? std::string("All translation replacements failed.") : last_error;
    }
    return replaced > 0;
  }

  // Permanently disable a shader by ucode hash. The hash is remembered even if
  // the shader hasn't been seen yet -- when the backend later loads a shader
  // matching a blacklisted hash it will be marked disabled immediately. Any
  // already-loaded matching shader is also disabled now. Thread-safe.
  //
  // Defined inline for the same cross-DLL-boundary reason as ReplaceShaderHLSL
  // above.
  void AddShaderBlacklist(uint64_t ucode_hash) {
    {
      std::lock_guard<std::mutex> lock(shader_blacklist_mutex_);
      shader_blacklist_.insert(ucode_hash);
    }
    // If the shader is already loaded in the backend, disable it now too.
    SetShaderDisabledByHash(ucode_hash, true);
  }
  void RemoveShaderBlacklist(uint64_t ucode_hash) {
    {
      std::lock_guard<std::mutex> lock(shader_blacklist_mutex_);
      shader_blacklist_.erase(ucode_hash);
    }
    SetShaderDisabledByHash(ucode_hash, false);
  }
  bool IsShaderBlacklisted(uint64_t ucode_hash) const {
    std::lock_guard<std::mutex> lock(shader_blacklist_mutex_);
    return shader_blacklist_.find(ucode_hash) != shader_blacklist_.end();
  }
  std::vector<uint64_t> GetShaderBlacklist() const {
    std::lock_guard<std::mutex> lock(shader_blacklist_mutex_);
    return std::vector<uint64_t>(shader_blacklist_.begin(), shader_blacklist_.end());
  }

  // Per-shader CPU-time profiling. The shader debugger overlay enables this
  // while its window is open and disables it on close so there is no overhead
  // in the common case. Thread-safe / lock-free on the hot path.
  bool IsShaderProfilingEnabled() const {
    return shader_profiling_enabled_.load(std::memory_order_relaxed);
  }
  void SetShaderProfilingEnabled(bool enabled) {
    shader_profiling_enabled_.store(enabled, std::memory_order_relaxed);
  }
  // Zero out per-shader counters. Default no-op -- backends with shader caches
  // override to walk every tracked shader.
  virtual void ResetShaderProfiling() {}

  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(std::function<void()> fn);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect GetDesiredSwapPostEffect() const { return swap_post_effect_desired_; }
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  // Implementations must not make assumptions that the front buffer will
  // necessarily be a resolve destination - it may be a texture generated by any
  // means like written to by the CPU or loaded from a file (the disclaimer
  // screen right in the beginning of 4D530AA4 is not a resolved render target,
  // for instance).
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) = 0;

  // May be called not only from the command processor thread when the command
  // processor is paused, and the termination of this function may be explicitly
  // awaited.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking);

  // Texture/shader mod replacement and dumping. Default: no-op. Backends
  // that support content-hash-keyed asset substitution (D3D12/Vulkan Xenos
  // command processors) override this to plumb the config down to their
  // texture cache and pipeline cache.
  virtual void InitializeAssetReplacement(const system::AssetReplacementConfig& config) {
    (void)config;
  }

  virtual void RequestFrameTrace(const std::filesystem::path& root_path);
  virtual void BeginTracing(const std::filesystem::path& root_path);
  virtual void EndTracing();

  // Captures the next full guest-rendered frame with RenderDoc, if attached.
  // Default: no-op. Backends with a RenderDoc integration (currently D3D12
  // only) override this.
  virtual void RequestRenderDocCapture() {}

  virtual void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) = 0;

  void RestoreRegisters(uint32_t first_register, const uint32_t* register_values,
                        uint32_t register_count, bool execute_callbacks);
  void RestoreGammaRamp(const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
                        const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
                        uint32_t new_gamma_ramp_rw_component);
  virtual void RestoreEdramSnapshot(const void* snapshot) = 0;

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value);

  void ExecutePacket(uint32_t ptr, uint32_t count);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  void WorkerThreadMain();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;

  virtual void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadRegisterValue(uint32_t index) const;
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  virtual void WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);
  void WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);

  const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const { return gamma_ramp_pwl_rgb_[0]; }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  uint32_t ExecutePrimaryBuffer(uint32_t start_index, uint32_t end_index);
  virtual void OnPrimaryBufferEnd() {}
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t length);
  bool ExecutePacket(memory::RingBuffer* reader);
  bool ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  virtual bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count);
  bool ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet, const char* opcode_name,
                              uint32_t viz_query_condition, uint32_t count_remaining);
  bool ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                        uint32_t count);
  bool ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                            uint32_t count);
  bool ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader, uint32_t packet,
                                               uint32_t count);
  bool ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,

                                            uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader, uint32_t packet,
                                           uint32_t count);
  bool ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet, uint32_t count);

  virtual Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                             const uint32_t* host_address, uint32_t dword_count) = 0;

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         IndexBufferInfo* index_buffer_info, bool major_mode_explicit) = 0;
  virtual bool IssueCopy() = 0;

  // "Actual" is for the command processor thread, to be read by the
  // implementations.
  SwapPostEffect GetActualSwapPostEffect() const { return swap_post_effect_actual_; }

  virtual void InitializeTrace();

  // Shared readback resolve mode with backend legacy-flag alias support.
  ReadbackResolveMode GetReadbackResolveMode(bool legacy_readback_resolve_enabled) const;
  // Shared memexport readback enable state with backend legacy-flag override support.
  bool IsReadbackMemexportEnabled(bool legacy_backend_flag) const;

  memory::Memory* memory_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  RegisterFile* register_file_ = nullptr;

  TraceWriter trace_writer_;
  enum class TraceState {
    kDisabled,
    kStreaming,
    kSingleFrame,
  };
  TraceState trace_state_ = TraceState::kDisabled;
  std::filesystem::path trace_stream_path_;
  std::filesystem::path trace_frame_path_;

  std::atomic<bool> worker_running_;
  system::object_ref<system::XHostThread> worker_thread_;

  std::queue<std::function<void()>> pending_fns_;

  // MicroEngine binary from PM4_ME_INIT
  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<rex::thread::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;

  // Some titles submit writes beyond the emulated register file range in PM4
  // packets. Preserve these values so dependent packet logic can still observe
  // them instead of dropping the write entirely.
  std::unordered_map<uint32_t, uint32_t> extended_register_values_;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  bool paused_ = false;

  // By default (such as for tools), post-processing is disabled.
  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

  // Set by backend command processors to their legacy memexport readback cvar
  // name (for explicit-override compatibility).
  const char* legacy_readback_memexport_cvar_name_ = nullptr;

 private:
  reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  uint32_t gamma_ramp_rw_component_ = 0;

  // Permanently-disabled shader hashes. Consulted by pipeline caches when
  // loading a shader for the first time.
  mutable std::mutex shader_blacklist_mutex_;
  std::unordered_set<uint64_t> shader_blacklist_;

  // Whether per-shader timing should be sampled in IssueDraw. Hot path reads
  // this every draw, so it's a relaxed atomic.
  std::atomic<bool> shader_profiling_enabled_{false};
};

}  // namespace rex::graphics
