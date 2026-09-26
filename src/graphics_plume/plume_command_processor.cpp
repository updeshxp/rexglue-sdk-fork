/**
 * @file        graphics_plume/plume_command_processor.cpp
 * @brief       PlumeCommandProcessor implementation
 */

#include "plume_command_processor.h"
#include "plume_graphics_system.h"
#include "plume_shader.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <xxhash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/system/xmemory.h>

namespace rex::graphics_plume {

PlumeCommandProcessor::PlumeCommandProcessor(PlumeGraphicsSystem* graphics_system,
                                             rex::system::KernelState* kernel_state,
                                             ::plume::RenderDevice* device)
    : rex::graphics::CommandProcessor(reinterpret_cast<rex::graphics::GraphicsSystem*>(graphics_system),
                                     kernel_state),
      plume_graphics_system_(graphics_system),
      plume_device_(device) {
  transpiler_.SetEnabled(true);
}

PlumeCommandProcessor::~PlumeCommandProcessor() = default;

bool PlumeCommandProcessor::Initialize() {
  REXLOG_INFO("PlumeCommandProcessor::Initialize");
  if (!rex::graphics::CommandProcessor::Initialize()) {
    REXLOG_ERROR("PlumeCommandProcessor: base Initialize failed");
    return false;
  }
  
  // PlumeTextureCache precisa que o registrador base e memoria sejam acessiveis
  shared_memory_ = std::make_unique<PlumeSharedMemory>(*memory_);
  texture_cache_ = std::make_unique<PlumeTextureCache>(*register_file_, *shared_memory_, 1, 1, *this, plume_device_);
  if (!texture_cache_->Initialize()) {
    REXLOG_ERROR("PlumeTextureCache::Initialize failed");
    return false;
  }
  
  render_target_cache_ = std::make_unique<PlumeRenderTargetCache>(
      *register_file_, *memory_, plume_device_, 1, 1);
  if (!render_target_cache_->Initialize()) {
    REXLOG_ERROR("PlumeRenderTargetCache::Initialize failed");
    return false;
  }
  
  return true;
}

void PlumeCommandProcessor::Shutdown() {
  REXLOG_INFO("PlumeCommandProcessor::Shutdown");
  render_target_cache_.reset();
  texture_cache_.reset();
  shared_memory_.reset();
  rex::graphics::CommandProcessor::Shutdown();
}

bool PlumeCommandProcessor::SetupContext() {
  REXLOG_INFO("PlumeCommandProcessor::SetupContext");
  if (!rex::graphics::CommandProcessor::SetupContext()) {
    return false;
  }

  if (plume_device_) {
    plume_queue_ = plume_device_->createCommandQueue(::plume::RenderCommandListType::DIRECT);
    if (plume_queue_) {
      for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        frames_[i].cmd_list = plume_queue_->createCommandList();
        frames_[i].fence = plume_device_->createCommandFence();
        frames_[i].in_flight = false;
        frames_[i].garbage.clear();
      }
      current_frame_index_ = 0;
      REXLOG_INFO("PlumeCommandProcessor: direct command queue and command lists/fences created");
    } else {
      REXLOG_WARN("PlumeCommandProcessor: could not create direct command queue");
    }

    // Set 0: Shared Memory SSBO (256 MB para acomodar dados de vertices e constantes do guest)
    constexpr size_t kSharedMemorySize = 256 * 1024 * 1024;
    shared_memory_buf_ = plume_device_->createBuffer(
        ::plume::RenderBufferDesc::UploadBuffer(kSharedMemorySize, ::plume::RenderBufferFlag::STORAGE));
    if (shared_memory_ && shared_memory_buf_) {
      shared_memory_->SetSharedMemoryBuffer(shared_memory_buf_.get(), kSharedMemorySize);
    }
    ::plume::RenderDescriptorRange set0_range(
        ::plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
    ::plume::RenderDescriptorSetDesc set0_desc(&set0_range, 1);
    shared_memory_descriptor_set_ = plume_device_->createDescriptorSet(set0_desc);
    if (shared_memory_descriptor_set_ && shared_memory_buf_) {
      shared_memory_descriptor_set_->setBuffer(0, shared_memory_buf_.get());
    }

    // Fallback 1x1 Dummy Texture and View
    ::plume::RenderTextureDesc dummy_tex_desc = ::plume::RenderTextureDesc::Texture(
        ::plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 1,
        ::plume::RenderFormat::R8G8B8A8_UNORM, ::plume::RenderTextureFlag::NONE);
    dummy_texture_ = plume_device_->createTexture(dummy_tex_desc);
    if (dummy_texture_) {
      ::plume::RenderTextureViewDesc dummy_view_desc;
      dummy_view_desc.dimension = ::plume::RenderTextureViewDimension::TEXTURE_2D;
      dummy_view_desc.format = ::plume::RenderFormat::R8G8B8A8_UNORM;
      dummy_view_desc.mipSlice = 0;
      dummy_view_desc.mipLevels = 1;
      dummy_view_desc.arrayIndex = 0;
      dummy_view_desc.arraySize = 1;
      dummy_texture_view_ = dummy_texture_->createTextureView(dummy_view_desc);
    }

    // Fallback Default Sampler
    ::plume::RenderSamplerDesc default_samp_desc;
    default_sampler_ = plume_device_->createSampler(default_samp_desc);
  }

  return true;
}

void PlumeCommandProcessor::ShutdownContext() {
  REXLOG_INFO("PlumeCommandProcessor::ShutdownContext");
  if (plume_queue_) {
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
      if (frames_[i].in_flight && frames_[i].fence) {
        plume_queue_->waitForCommandFence(frames_[i].fence.get());
      }
      frames_[i].cmd_list.reset();
      frames_[i].fence.reset();
      frames_[i].garbage.clear();
    }
  }
  plume_queue_.reset();
  graphics_pipelines_.clear();
  pipeline_layouts_.clear();
  if (shared_memory_) {
    shared_memory_->SetSharedMemoryBuffer(nullptr, 0);
  }
  shared_memory_descriptor_set_.reset();
  shared_memory_buf_.reset();
  dummy_texture_view_.reset();
  dummy_texture_.reset();
  default_sampler_.reset();
  rex::graphics::CommandProcessor::ShutdownContext();
}

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  REXLOG_DEBUG("PlumeCommandProcessor::IssueSwap: ptr=0x{:08X}, {}x{}",
               frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  auto& frame = frames_[current_frame_index_];
  if (!frame.cmd_list || !plume_queue_) {
    plume_graphics_system_->Present();
    return;
  }

  // -------------------------------------------------------------------------
  // Fase D: blit do color render target -> swapchain texture
  // -------------------------------------------------------------------------
  // Se o command list não estiver aberto (nenhum draw ocorreu neste frame),
  // abre-o agora para a operação de copia.
  if (!cmd_list_open_) {
    frame.cmd_list->begin();
    cmd_list_open_ = true;
  }

  auto* swapchain = plume_graphics_system_->plume_swapchain();
  if (swapchain) {
    uint32_t texture_index = 0;
    if (swapchain->acquireTexture(nullptr, &texture_index)) {
      ::plume::RenderTexture* swap_tex = swapchain->getTexture(texture_index);
      ::plume::RenderTexture* color_rt = render_target_cache_ ? render_target_cache_->MVP_GetColorTexture() : nullptr;
      
      if (swap_tex && color_rt) {
        ::plume::RenderTextureBarrier barriers_pre[2];
        barriers_pre[0] = ::plume::RenderTextureBarrier(color_rt, ::plume::RenderTextureLayout::COPY_SOURCE);
        barriers_pre[1] = ::plume::RenderTextureBarrier(swap_tex, ::plume::RenderTextureLayout::COPY_DEST);
        frame.cmd_list->barriers(::plume::RenderBarrierStage::ALL, nullptr, 0, barriers_pre, 2);

        frame.cmd_list->copyTexture(swap_tex, color_rt);

        ::plume::RenderTextureBarrier barriers_post[2];
        barriers_post[0] = ::plume::RenderTextureBarrier(swap_tex, ::plume::RenderTextureLayout::PRESENT);
        barriers_post[1] = ::plume::RenderTextureBarrier(color_rt, ::plume::RenderTextureLayout::COLOR_WRITE);
        frame.cmd_list->barriers(::plume::RenderBarrierStage::ALL, nullptr, 0, barriers_post, 2);
      } else if (swap_tex) {
        // Fallback
        ::plume::RenderTextureBarrier barriers_post[1];
        barriers_post[0] = ::plume::RenderTextureBarrier(swap_tex, ::plume::RenderTextureLayout::PRESENT);
        frame.cmd_list->barriers(::plume::RenderBarrierStage::ALL, nullptr, 0, barriers_post, 1);
      }
    }

    frame.cmd_list->end();
    cmd_list_open_ = false;
    const ::plume::RenderCommandList* lists[] = { frame.cmd_list.get() };
    plume_queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
    frame.in_flight = true;

    // Present
    if (swapchain) {
      swapchain->present(texture_index, nullptr, 0);
    }
  } else {
    // Sem swapchain ou sem framebuffer ainda: apenas fecha e submete vazio
    frame.cmd_list->end();
    cmd_list_open_ = false;
    const ::plume::RenderCommandList* lists[] = { frame.cmd_list.get() };
    plume_queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
    frame.in_flight = true;
    plume_graphics_system_->Present();
  }
  
  // Mover para o próximo frame
  current_frame_index_ = (current_frame_index_ + 1) % kMaxFramesInFlight;
  auto& next_frame = frames_[current_frame_index_];
  
  // Esperar a GPU terminar de renderizar o "next_frame" anterior, se estiver em voo
  if (next_frame.in_flight && next_frame.fence) {
    plume_queue_->waitForCommandFence(next_frame.fence.get());
    next_frame.in_flight = false;
  }
  
  // Agora podemos limpar o lixo do próximo frame em segurança
  next_frame.garbage.clear();
  if (render_target_cache_) {
    render_target_cache_->EndFrame();
  }
  
  if (texture_cache_) {
    texture_cache_->EndFrame();
    texture_cache_->BeginSubmission(0); // Dummy sub index
  }

}

rex::graphics::Shader* PlumeCommandProcessor::LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                                         uint32_t guest_address,
                                                         const uint32_t* host_address,
                                                         uint32_t dword_count) {
  transpiler_.ObserveShaderLoad(static_cast<uint32_t>(shader_type), guest_address, dword_count);

  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = loaded_shaders_.find(hash);
  if (it != loaded_shaders_.end()) {
    return it->second.get();
  }

  auto shader = std::make_unique<PlumeShader>(
      plume_device_,
      shader_type,
      hash,
      host_address,
      dword_count,
      std::endian::big);

  auto* ptr = shader.get();
  loaded_shaders_[hash] = std::move(shader);
  return ptr;
}

bool PlumeCommandProcessor::IssueDraw(rex::graphics::xenos::PrimitiveType prim_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  (void)major_mode_explicit;
  transpiler_.ObserveDraw("DRAW", static_cast<uint32_t>(prim_type), index_count, index_buffer_info != nullptr);

  ::plume::RenderPrimitiveTopology topology = ::plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  switch (prim_type) {
    case rex::graphics::xenos::PrimitiveType::kPointList: topology = ::plume::RenderPrimitiveTopology::POINT_LIST; break;
    case rex::graphics::xenos::PrimitiveType::kLineList: topology = ::plume::RenderPrimitiveTopology::LINE_LIST; break;
    case rex::graphics::xenos::PrimitiveType::kLineStrip: topology = ::plume::RenderPrimitiveTopology::LINE_STRIP; break;
    case rex::graphics::xenos::PrimitiveType::kTriangleList: topology = ::plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case rex::graphics::xenos::PrimitiveType::kTriangleStrip: topology = ::plume::RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case rex::graphics::xenos::PrimitiveType::kTriangleFan: topology = ::plume::RenderPrimitiveTopology::TRIANGLE_FAN; break;
    default: topology = ::plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
  }

  auto pa_su_sc_mode_cntl = register_file_->Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
  if (pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
    return true; // Drop invisible draw
  }

  // -------------------------------------------------------------------------
  // Fase C: Framebuffer — Color Target + Depth Target
  // -------------------------------------------------------------------------
  uint32_t fb_width = 1280u;
  uint32_t fb_height = 720u;
  ::plume::RenderFormat color_fmt = ::plume::RenderFormat::B8G8R8A8_UNORM;
  ::plume::RenderFormat depth_fmt = ::plume::RenderFormat::D32_FLOAT_S8_UINT;

  {
    auto rb_surface = register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>();
    auto rb_color   = register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>();
    auto rb_depth   = register_file_->Get<rex::graphics::reg::RB_DEPTH_INFO>();

    fb_width  = (rb_surface.surface_pitch > 0) ? rb_surface.surface_pitch : 1280u;
    {
      auto scissor_br = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_SCISSOR_BR>();
      if (scissor_br.br_y > 0) fb_height = scissor_br.br_y;
    }

    auto xenos_color_fmt = rb_color.color_format;
    switch (xenos_color_fmt) {
      case rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        color_fmt = ::plume::RenderFormat::R8G8B8A8_UNORM; break;
      case rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        color_fmt = ::plume::RenderFormat::R16G16B16A16_UNORM; break;
      case rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      case rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        color_fmt = ::plume::RenderFormat::R16G16B16A16_FLOAT; break;
      case rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        color_fmt = ::plume::RenderFormat::R16G16_FLOAT; break;
      case rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT:
        color_fmt = ::plume::RenderFormat::R32_FLOAT; break;
      case rex::graphics::xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        color_fmt = ::plume::RenderFormat::R32G32_FLOAT; break;
      default:
        color_fmt = ::plume::RenderFormat::R8G8B8A8_UNORM; break;
    }
  }

  auto pipe = GetOrCreateGraphicsPipeline(topology, color_fmt);
  if (!pipe.pipeline || !pipe.layout || !GetActiveCommandList()) {
    return false;
  }

  // -------------------------------------------------------------------------
  // Fase D.1: Begin lazy do command list (uma vez por frame)
  // -------------------------------------------------------------------------
  if (!cmd_list_open_) {
    GetActiveCommandList()->begin();
    cmd_list_open_ = true;
    if (texture_cache_) {
      texture_cache_->BeginFrame();
    }
  }

  if (render_target_cache_) {
    // Para MVP, o framebuffer eh resolvido pelo PlumeRenderTargetCache.
    ::plume::RenderFramebuffer* fb = render_target_cache_->MVP_GetOrCreateFramebuffer(plume_device_, fb_width, fb_height, color_fmt, depth_fmt);
    if (fb) GetActiveCommandList()->setFramebuffer(fb);
  }

  // -------------------------------------------------------------------------
  // Fase B.1: Viewport e Scissor
  // -------------------------------------------------------------------------
  {
    // Viewport: lemos os registros PA_CL_VPORT_* (xscale/xoffset/yscale/yoffset/zscale/zoffset)
    // No Xenos: x = offset - |scale|, width = 2 * |scale|
    float vport_xscale  = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE]);
    float vport_xoffset = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET]);
    float vport_yscale  = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE]);
    float vport_yoffset = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET]);
    float vport_zscale  = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE]);
    float vport_zoffset = rex::memory::Reinterpret<float>((*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]);

    float vp_x = vport_xoffset - std::abs(vport_xscale);
    float vp_y = vport_yoffset - std::abs(vport_yscale);
    float vp_w = 2.0f * std::abs(vport_xscale);
    float vp_h = 2.0f * std::abs(vport_yscale);
    float vp_min_z = vport_zoffset - std::abs(vport_zscale);
    float vp_max_z = vport_zoffset + std::abs(vport_zscale);

    ::plume::RenderViewport viewport;
    viewport.x = vp_x;
    viewport.y = vp_y;
    viewport.width  = (vp_w > 0.0f) ? vp_w : 1280.0f;
    viewport.height = (vp_h > 0.0f) ? vp_h : 720.0f;
    viewport.minDepth = vp_min_z;
    viewport.maxDepth = vp_max_z;
    GetActiveCommandList()->setViewports(&viewport, 1);

    // Scissor: lemos PA_SC_WINDOW_SCISSOR_TL/BR
    auto scissor_tl = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto scissor_br = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_SCISSOR_BR>();

    ::plume::RenderRect scissor;
    scissor.left   = static_cast<int32_t>(scissor_tl.tl_x);
    scissor.top    = static_cast<int32_t>(scissor_tl.tl_y);
    scissor.right  = static_cast<int32_t>(scissor_br.br_x);
    scissor.bottom = static_cast<int32_t>(scissor_br.br_y);

    // Se scissor for zero/inválido, usar um rect cobrindo todo o viewport
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
      scissor.left   = 0;
      scissor.top    = 0;
      scissor.right  = static_cast<int32_t>(viewport.width);
      scissor.bottom = static_cast<int32_t>(viewport.height);
    }
    GetActiveCommandList()->setScissors(&scissor, 1);
  }

  // -------------------------------------------------------------------------
  // Fase B.2: Vertex Buffers — iteramos os fetch constants do vertex shader
  // -------------------------------------------------------------------------
  {
    auto* vs = static_cast<PlumeShader*>(active_vertex_shader_);
    if (vs && memory_) {
      const rex::graphics::Shader::ConstantRegisterMap& const_map = vs->constant_register_map();
      uint32_t num_words = static_cast<uint32_t>(
          rex::countof(const_map.vertex_fetch_bitmap));

      // Coletamos os views e slots
      std::vector<::plume::RenderVertexBufferView> vb_views;
      std::vector<::plume::RenderInputSlot>        vb_slots;
      // Mantemos os buffers vivos até o fim do draw
      std::vector<std::unique_ptr<::plume::RenderBuffer>> vb_buffers;

      for (uint32_t i = 0; i < num_words; ++i) {
        uint32_t bits = const_map.vertex_fetch_bitmap[i];
        uint32_t j;
        while (rex::bit_scan_forward(bits, &j)) {
          bits &= ~(uint32_t(1) << j);
          uint32_t vfetch_index = i * 32 + j;

          rex::graphics::xenos::xe_gpu_vertex_fetch_t vfetch =
              register_file_->GetVertexFetch(vfetch_index);

          if (vfetch.type != rex::graphics::xenos::FetchConstantType::kVertex &&
              vfetch.type != rex::graphics::xenos::FetchConstantType::kInvalidVertex) {
            REXLOG_WARN("PlumeCommandProcessor: vfetch constant {} has invalid type, skipping",
                        vfetch_index);
            continue;
          }

          // O endereço está em dwords, o size está em words (shorts)
          uint32_t guest_addr_bytes = vfetch.address << 2;
          uint32_t size_bytes       = vfetch.size << 2;
          if (size_bytes == 0) {
            continue;
          }

          // Traduz endereço guest físico -> host
          const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(guest_addr_bytes);
          if (!src) {
            REXLOG_WARN("PlumeCommandProcessor: vfetch constant {} address 0x{:08X} invalid",
                        vfetch_index, guest_addr_bytes);
            continue;
          }

          if (shared_memory_) {
            shared_memory_->RequestRange(guest_addr_bytes, size_bytes);
          }

          // Cria RenderBuffer UPLOAD para este vertex buffer
          auto buf_desc = ::plume::RenderBufferDesc::VertexBuffer(
              size_bytes,
              ::plume::RenderHeapType::UPLOAD);
          auto plume_buf = plume_device_->createBuffer(buf_desc);
          if (!plume_buf) {
            REXLOG_WARN("PlumeCommandProcessor: failed to create vertex buffer for vfetch {}",
                        vfetch_index);
            continue;
          }

          // Copia os dados
          void* mapped = plume_buf->map();
          if (mapped) {
            std::memcpy(mapped, src, size_bytes);
            plume_buf->unmap();
          }

          uint32_t slot = static_cast<uint32_t>(vb_views.size());
          ::plume::RenderVertexBufferView view(
              plume_buf->at(0),
              size_bytes);
          vb_views.push_back(view);

          // stride 0 — raw byte-addressable buffer (o shader usa vfetch com endereço absoluto)
          ::plume::RenderInputSlot input_slot(
              slot,
              0,
              ::plume::RenderInputSlotClassification::PER_VERTEX_DATA);
          vb_slots.push_back(input_slot);
          vb_buffers.push_back(std::move(plume_buf));
        }
      }

      if (!vb_views.empty()) {
        GetActiveCommandList()->setVertexBuffers(
            0,
            vb_views.data(),
            static_cast<uint32_t>(vb_views.size()),
            vb_slots.data());
      }
    }
  }

  // -------------------------------------------------------------------------
  // Fase B.3: Index Buffer
  // -------------------------------------------------------------------------
  std::unique_ptr<::plume::RenderBuffer> ib_buffer;
  if (index_buffer_info != nullptr && index_buffer_info->count > 0 && memory_) {
    // Tamanho de cada index
    bool is_32bit = (index_buffer_info->format ==
                     rex::graphics::xenos::IndexFormat::kInt32);
    uint32_t index_stride = is_32bit ? 4u : 2u;
    uint32_t ib_size_bytes = index_count * index_stride;

    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(
        index_buffer_info->guest_base);

    if (src && ib_size_bytes > 0) {
      if (shared_memory_) {
        shared_memory_->RequestRange(index_buffer_info->guest_base, ib_size_bytes);
      }
      auto ib_desc = ::plume::RenderBufferDesc::IndexBuffer(
          ib_size_bytes,
          ::plume::RenderHeapType::UPLOAD);
      ib_buffer = plume_device_->createBuffer(ib_desc);
      if (ib_buffer) {
        void* mapped = ib_buffer->map();
        if (mapped) {
          std::memcpy(mapped, src, ib_size_bytes);
          ib_buffer->unmap();
        }
        ::plume::RenderFormat ib_format = is_32bit
            ? ::plume::RenderFormat::R32_UINT
            : ::plume::RenderFormat::R16_UINT;
        ::plume::RenderIndexBufferView ib_view(
            ib_buffer->at(0),
            ib_size_bytes,
            ib_format);
        GetActiveCommandList()->setIndexBuffer(&ib_view);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Fase G, F & H: Viewport, Scissor, Uniforms e Texturas
  // -------------------------------------------------------------------------
  PlumeFrameContext::FrameGarbage garbage;

  if (texture_cache_) {
    uint32_t used_textures = 0;
    if (active_pixel_shader_) {
      auto* ps = static_cast<PlumeShader*>(active_pixel_shader_);
      used_textures |= ps->GetUsedTextureMaskAfterTranslation();
    }
    if (active_vertex_shader_) {
      auto* vs = static_cast<PlumeShader*>(active_vertex_shader_);
      used_textures |= vs->GetUsedTextureMaskAfterTranslation();
    }
    texture_cache_->RequestTextures(used_textures);
  }

  rex::graphics::draw_util::ViewportInfo viewport_info;
  auto rb_depthcontrol = register_file_->Get<rex::graphics::reg::RB_DEPTHCONTROL>();
  
  // Utiliza a utilidade nativa do Xenos para calcular os recortes corretamente
  rex::graphics::draw_util::GetHostViewportInfo(
      *register_file_, 1, 1, false,
      fb_width, fb_height, true,
      rb_depthcontrol, false, false,
      active_pixel_shader_ && active_pixel_shader_->writes_depth(),
      viewport_info);

  rex::graphics::draw_util::Scissor scissor_info;
  rex::graphics::draw_util::GetScissor(*register_file_, scissor_info, true);

  // Aplica Viewport no Command List (Plume)
  ::plume::RenderViewport vp;
  vp.x = static_cast<float>(viewport_info.xy_offset[0]);
  vp.y = static_cast<float>(viewport_info.xy_offset[1]);
  vp.width = static_cast<float>(viewport_info.xy_extent[0]);
  vp.height = static_cast<float>(viewport_info.xy_extent[1]);
  vp.minDepth = viewport_info.z_min;
  vp.maxDepth = viewport_info.z_max;
  GetActiveCommandList()->setViewports(vp);

  // Aplica Scissor no Command List (Plume)
  ::plume::RenderRect sc;
  sc.left = scissor_info.offset[0];
  sc.top = scissor_info.offset[1];
  sc.right = scissor_info.offset[0] + scissor_info.extent[0];
  sc.bottom = scissor_info.offset[1] + scissor_info.extent[1];
  GetActiveCommandList()->setScissors(sc);
  
  // Atualiza Constantes de Sistema (NDC scale/offset basico)
  system_constants_.ndc_scale[0] = viewport_info.ndc_scale[0];
  system_constants_.ndc_scale[1] = viewport_info.ndc_scale[1];
  system_constants_.ndc_scale[2] = viewport_info.ndc_scale[2];
  system_constants_.ndc_offset[0] = viewport_info.ndc_offset[0];
  system_constants_.ndc_offset[1] = viewport_info.ndc_offset[1];
  system_constants_.ndc_offset[2] = viewport_info.ndc_offset[2];

  garbage.sys_buf = plume_device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(sizeof(system_constants_), ::plume::RenderBufferFlag::CONSTANT));
  void* sys_ptr = garbage.sys_buf->map();
  if(sys_ptr) {
      std::memcpy(sys_ptr, &system_constants_, sizeof(system_constants_));
      garbage.sys_buf->unmap();
  }

  // Float Constants (512 vec4s)
  size_t float_size = 512 * 4 * sizeof(float);
  const void* float_src = &register_file_->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X];
  
  garbage.vs_float_buf = plume_device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(float_size, ::plume::RenderBufferFlag::CONSTANT));
  void* vs_ptr = garbage.vs_float_buf->map();
  if(vs_ptr) {
      std::memcpy(vs_ptr, float_src, float_size);
      garbage.vs_float_buf->unmap();
  }
  
  garbage.ps_float_buf = plume_device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(float_size, ::plume::RenderBufferFlag::CONSTANT));
  void* ps_ptr = garbage.ps_float_buf->map();
  if(ps_ptr) {
      std::memcpy(ps_ptr, float_src, float_size);
      garbage.ps_float_buf->unmap();
  }

  // Bool/Loop Constants (256 bytes)
  garbage.bool_buf = plume_device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(256, ::plume::RenderBufferFlag::CONSTANT));
  
  // Fetch Constants (32 * 6 dwords = 768 bytes)
  garbage.fetch_buf = plume_device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(768, ::plume::RenderBufferFlag::CONSTANT));

  // Cria e Preenche o Descriptor Set (set 1 = kDescriptorSetConstants)
  ::plume::RenderDescriptorRange ranges[5];
  for(int i = 0; i < 5; i++) {
      ranges[i].type = ::plume::RenderDescriptorRangeType::CONSTANT_BUFFER;
      ranges[i].count = 1;
      ranges[i].binding = i;
  }
  ::plume::RenderDescriptorSetDesc set_desc(ranges, 5);
  garbage.descriptor_set = plume_device_->createDescriptorSet(set_desc);
  
  if (garbage.descriptor_set) {
      garbage.descriptor_set->setBuffer(0, garbage.sys_buf.get());
      garbage.descriptor_set->setBuffer(1, garbage.vs_float_buf.get());
      garbage.descriptor_set->setBuffer(2, garbage.ps_float_buf.get());
      garbage.descriptor_set->setBuffer(3, garbage.bool_buf.get());
      garbage.descriptor_set->setBuffer(4, garbage.fetch_buf.get());
  }

  // -------------------------------------------------------------------------
  // Fase B.4: Pipeline, Descriptors e Draw Call
  // -------------------------------------------------------------------------
  GetActiveCommandList()->setGraphicsPipelineLayout(pipe.layout);
  GetActiveCommandList()->setPipeline(pipe.pipeline);

  if (shared_memory_descriptor_set_) {
    GetActiveCommandList()->setGraphicsDescriptorSet(shared_memory_descriptor_set_.get(), 0);
  }

  if (garbage.descriptor_set) {
    GetActiveCommandList()->setGraphicsDescriptorSet(garbage.descriptor_set.get(), 1);
  }

  // Set 2: Vertex Shader Textures & Samplers
  auto* vs = static_cast<PlumeShader*>(active_vertex_shader_);
  uint32_t t_vs = vs ? static_cast<uint32_t>(vs->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t s_vs = vs ? static_cast<uint32_t>(vs->GetSamplerBindingsAfterTranslation().size()) : 0;
  if (t_vs + s_vs > 0) {
    std::vector<::plume::RenderDescriptorRange> set2_ranges;
    set2_ranges.reserve(t_vs + s_vs);
    for (uint32_t i = 0; i < t_vs; ++i) {
      set2_ranges.emplace_back(::plume::RenderDescriptorRangeType::TEXTURE, i, 1);
    }
    for (uint32_t i = 0; i < s_vs; ++i) {
      set2_ranges.emplace_back(::plume::RenderDescriptorRangeType::SAMPLER, t_vs + i, 1);
    }
    ::plume::RenderDescriptorSetDesc set2_desc(set2_ranges.data(), static_cast<uint32_t>(set2_ranges.size()));
    garbage.vs_descriptor_set = plume_device_->createDescriptorSet(set2_desc);
    if (garbage.vs_descriptor_set) {
      const auto& vs_textures = vs->GetTextureBindingsAfterTranslation();
      for (uint32_t i = 0; i < t_vs; ++i) {
        auto* plume_tex = texture_cache_ ? texture_cache_->GetActiveBindingPlumeTexture(vs_textures[i].fetch_constant) : nullptr;
        if (plume_tex && plume_tex->plume_texture()) {
          garbage.vs_descriptor_set->setTexture(i, plume_tex->plume_texture(), ::plume::RenderTextureLayout::SHADER_READ, plume_tex->plume_srv());
        } else if (dummy_texture_) {
          garbage.vs_descriptor_set->setTexture(i, dummy_texture_.get(), ::plume::RenderTextureLayout::SHADER_READ, dummy_texture_view_.get());
        }
      }
      const auto& vs_samplers = vs->GetSamplerBindingsAfterTranslation();
      for (uint32_t i = 0; i < s_vs; ++i) {
        auto params = texture_cache_->GetSamplerParameters(vs_samplers[i]);
        auto* sampler = texture_cache_->UseSampler(params);
        if (!sampler) sampler = default_sampler_.get();
        if (sampler) {
          garbage.vs_descriptor_set->setSampler(t_vs + i, sampler);
        }
      }
      GetActiveCommandList()->setGraphicsDescriptorSet(garbage.vs_descriptor_set.get(), 2);
    }
  }

  // Set 3: Pixel Shader Textures & Samplers
  auto* ps = static_cast<PlumeShader*>(active_pixel_shader_);
  uint32_t t_ps = ps ? static_cast<uint32_t>(ps->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t s_ps = ps ? static_cast<uint32_t>(ps->GetSamplerBindingsAfterTranslation().size()) : 0;
  if (t_ps + s_ps > 0 && ps) {
    std::vector<::plume::RenderDescriptorRange> set3_ranges;
    set3_ranges.reserve(t_ps + s_ps);
    for (uint32_t i = 0; i < t_ps; ++i) {
      set3_ranges.emplace_back(::plume::RenderDescriptorRangeType::TEXTURE, i, 1);
    }
    for (uint32_t i = 0; i < s_ps; ++i) {
      set3_ranges.emplace_back(::plume::RenderDescriptorRangeType::SAMPLER, t_ps + i, 1);
    }
    ::plume::RenderDescriptorSetDesc set3_desc(set3_ranges.data(), static_cast<uint32_t>(set3_ranges.size()));
    garbage.ps_descriptor_set = plume_device_->createDescriptorSet(set3_desc);
    if (garbage.ps_descriptor_set) {
      const auto& ps_textures = ps->GetTextureBindingsAfterTranslation();
      for (uint32_t i = 0; i < t_ps; ++i) {
        auto* plume_tex = texture_cache_ ? texture_cache_->GetActiveBindingPlumeTexture(ps_textures[i].fetch_constant) : nullptr;
        if (plume_tex && plume_tex->plume_texture()) {
          garbage.ps_descriptor_set->setTexture(i, plume_tex->plume_texture(), ::plume::RenderTextureLayout::SHADER_READ, plume_tex->plume_srv());
        } else if (dummy_texture_) {
          garbage.ps_descriptor_set->setTexture(i, dummy_texture_.get(), ::plume::RenderTextureLayout::SHADER_READ, dummy_texture_view_.get());
        }
      }
      const auto& ps_samplers = ps->GetSamplerBindingsAfterTranslation();
      for (uint32_t i = 0; i < s_ps; ++i) {
        auto params = texture_cache_->GetSamplerParameters(ps_samplers[i]);
        auto* sampler = texture_cache_->UseSampler(params);
        if (!sampler) sampler = default_sampler_.get();
        if (sampler) {
          garbage.ps_descriptor_set->setSampler(t_ps + i, sampler);
        }
      }
      GetActiveCommandList()->setGraphicsDescriptorSet(garbage.ps_descriptor_set.get(), 3);
    }
  }

  frames_[current_frame_index_].garbage.push_back(std::move(garbage));

  if (ib_buffer && index_buffer_info != nullptr && index_buffer_info->count > 0) {
    GetActiveCommandList()->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else {
    GetActiveCommandList()->drawInstanced(index_count, 1, 0, 0);
  }

  return true;
}

bool PlumeCommandProcessor::IssueCopy() {
  if (!render_target_cache_) return false;

  uint32_t written_address = 0;
  uint32_t written_length = 0;
  
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                     written_address, written_length)) {
    return false;
  }

  return true;
}

::plume::RenderPipelineLayout* PlumeCommandProcessor::GetOrCreatePipelineLayout(
    uint32_t t_vs, uint32_t s_vs, uint32_t t_ps, uint32_t s_ps) {
  uint64_t layout_key = (static_cast<uint64_t>(t_vs) << 48) |
                        (static_cast<uint64_t>(s_vs) << 32) |
                        (static_cast<uint64_t>(t_ps) << 16) |
                        static_cast<uint64_t>(s_ps);

  auto it = pipeline_layouts_.find(layout_key);
  if (it != pipeline_layouts_.end()) {
    return it->second.get();
  }

  // Set 0: Shared Memory SSBO (binding 0)
  ::plume::RenderDescriptorRange set0_ranges[1] = {
      ::plume::RenderDescriptorRange(::plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1)
  };
  ::plume::RenderDescriptorSetDesc set0_desc(set0_ranges, 1);

  // Set 1: Constants (bindings 0..4)
  ::plume::RenderDescriptorRange set1_ranges[5];
  for (uint32_t i = 0; i < 5; ++i) {
    set1_ranges[i] = ::plume::RenderDescriptorRange(::plume::RenderDescriptorRangeType::CONSTANT_BUFFER, i, 1);
  }
  ::plume::RenderDescriptorSetDesc set1_desc(set1_ranges, 5);

  // Set 2: Vertex textures (bindings 0..t_vs-1) and samplers (bindings t_vs..t_vs+s_vs-1)
  std::vector<::plume::RenderDescriptorRange> set2_ranges;
  set2_ranges.reserve(t_vs + s_vs);
  for (uint32_t i = 0; i < t_vs; ++i) {
    set2_ranges.emplace_back(::plume::RenderDescriptorRangeType::TEXTURE, i, 1);
  }
  for (uint32_t i = 0; i < s_vs; ++i) {
    set2_ranges.emplace_back(::plume::RenderDescriptorRangeType::SAMPLER, t_vs + i, 1);
  }
  ::plume::RenderDescriptorSetDesc set2_desc(
      set2_ranges.empty() ? nullptr : set2_ranges.data(),
      static_cast<uint32_t>(set2_ranges.size()));

  // Set 3: Pixel textures (bindings 0..t_ps-1) and samplers (bindings t_ps..t_ps+s_ps-1)
  std::vector<::plume::RenderDescriptorRange> set3_ranges;
  set3_ranges.reserve(t_ps + s_ps);
  for (uint32_t i = 0; i < t_ps; ++i) {
    set3_ranges.emplace_back(::plume::RenderDescriptorRangeType::TEXTURE, i, 1);
  }
  for (uint32_t i = 0; i < s_ps; ++i) {
    set3_ranges.emplace_back(::plume::RenderDescriptorRangeType::SAMPLER, t_ps + i, 1);
  }
  ::plume::RenderDescriptorSetDesc set3_desc(
      set3_ranges.empty() ? nullptr : set3_ranges.data(),
      static_cast<uint32_t>(set3_ranges.size()));

  ::plume::RenderDescriptorSetDesc set_descs[4] = {
      set0_desc, set1_desc, set2_desc, set3_desc
  };
  ::plume::RenderPipelineLayoutDesc layout_desc(nullptr, 0, set_descs, 4);

  auto pipeline_layout = plume_device_->createPipelineLayout(layout_desc);
  if (!pipeline_layout) {
    REXLOG_ERROR("PlumeCommandProcessor: failed to create pipeline layout (t_vs={}, s_vs={}, t_ps={}, s_ps={})",
                 t_vs, s_vs, t_ps, s_ps);
    return nullptr;
  }

  auto* ptr = pipeline_layout.get();
  pipeline_layouts_[layout_key] = std::move(pipeline_layout);
  return ptr;
}

PlumeCommandProcessor::PlumePipeline PlumeCommandProcessor::GetOrCreateGraphicsPipeline(
    ::plume::RenderPrimitiveTopology topology, ::plume::RenderFormat color_format) {
  auto* vs = static_cast<PlumeShader*>(active_vertex_shader_);
  auto* ps = static_cast<PlumeShader*>(active_pixel_shader_);
  
  uint64_t hash = 0;
  if (vs) hash ^= vs->ucode_data_hash();
  if (ps) hash ^= (ps->ucode_data_hash() * 31);
  
  hash ^= static_cast<uint64_t>(topology) << 32;
  hash ^= static_cast<uint64_t>(color_format) << 48;

  auto pa_su_sc_mode_cntl = register_file_->Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
  hash ^= static_cast<uint64_t>(pa_su_sc_mode_cntl.value) << 16;
  
  auto rb_blendcontrol = register_file_->Get<rex::graphics::reg::RB_BLENDCONTROL>();
  hash ^= static_cast<uint64_t>(rb_blendcontrol.value) << 8;

  auto rb_depthcontrol = register_file_->Get<rex::graphics::reg::RB_DEPTHCONTROL>();
  hash ^= static_cast<uint64_t>(rb_depthcontrol.value) << 24;

  auto rb_colorcontrol = register_file_->Get<rex::graphics::reg::RB_COLORCONTROL>();
  hash ^= static_cast<uint64_t>(rb_colorcontrol.value) << 40;

  auto rb_color_mask = register_file_->Get<rex::graphics::reg::RB_COLOR_MASK>();
  hash ^= static_cast<uint64_t>(rb_color_mask.value) << 48;

  auto rb_stencilrefmask = register_file_->Get<rex::graphics::reg::RB_STENCILREFMASK>();
  hash ^= static_cast<uint64_t>(rb_stencilrefmask.value) << 56;

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (ps && vs) {
    interpolator_mask = vs->writes_interpolators() &
                        ps->GetInterpolatorInputMask(
                            register_file_->Get<rex::graphics::reg::SQ_PROGRAM_CNTL>(),
                            register_file_->Get<rex::graphics::reg::SQ_CONTEXT_MISC>(),
                            ps_param_gen_pos);
  }

  uint64_t vs_modification = 0;
  uint64_t ps_modification = 0;

  rex::graphics::SpirvShaderTranslator::Features features(false);
  features.spirv_version = spv::Spv_1_3;
  features.max_storage_buffer_range = UINT32_MAX;
  rex::graphics::SpirvShaderTranslator translator(features, false, false, false);

  if (vs) {
    rex::graphics::SpirvShaderTranslator::Modification vs_mod(
        translator.GetDefaultVertexShaderModification(
            vs->GetDynamicAddressableRegisterCount(register_file_->Get<rex::graphics::reg::SQ_PROGRAM_CNTL>().vs_num_reg)));
    vs_mod.vertex.interpolator_mask = interpolator_mask;
    vs_modification = vs_mod.value;
  }

  if (ps) {
    rex::graphics::SpirvShaderTranslator::Modification ps_mod(
        translator.GetDefaultPixelShaderModification(
            ps->GetDynamicAddressableRegisterCount(register_file_->Get<rex::graphics::reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask & ~rex::graphics::xenos::GetInterpolatorSamplingPattern(
                                register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>().msaa_samples,
                                register_file_->Get<rex::graphics::reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                register_file_->Get<rex::graphics::reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < rex::graphics::xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
    }
    ps_modification = ps_mod.value;
  }

  hash ^= vs_modification;
  hash ^= (ps_modification * 37);

  auto it = graphics_pipelines_.find(hash);
  if (it != graphics_pipelines_.end()) {
    return { it->second.pipeline.get(), it->second.layout };
  }

  ::plume::RenderGraphicsPipelineDesc desc;
  
  if (vs) desc.vertexShader = vs->GetOrCreatePlumeShader(vs_modification);
  if (ps) desc.pixelShader = ps->GetOrCreatePlumeShader(ps_modification);

  uint32_t t_vs = vs ? static_cast<uint32_t>(vs->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t s_vs = vs ? static_cast<uint32_t>(vs->GetSamplerBindingsAfterTranslation().size()) : 0;
  uint32_t t_ps = ps ? static_cast<uint32_t>(ps->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t s_ps = ps ? static_cast<uint32_t>(ps->GetSamplerBindingsAfterTranslation().size()) : 0;

  ::plume::RenderPipelineLayout* layout = GetOrCreatePipelineLayout(t_vs, s_vs, t_ps, s_ps);
  if (!layout) {
    return {};
  }
  desc.pipelineLayout = layout;

  desc.primitiveTopology = topology;
  desc.dynamicDepthBiasEnabled = true;

  if (pa_su_sc_mode_cntl.cull_front) {
    desc.cullMode = ::plume::RenderCullMode::FRONT;
  } else if (pa_su_sc_mode_cntl.cull_back) {
    desc.cullMode = ::plume::RenderCullMode::BACK;
  } else {
    desc.cullMode = ::plume::RenderCullMode::NONE;
  }

  desc.frontFace = pa_su_sc_mode_cntl.face ? ::plume::RenderFrontFace::CLOCKWISE : ::plume::RenderFrontFace::COUNTER_CLOCKWISE;

  desc.depthEnabled = rb_depthcontrol.z_enable;
  desc.depthWriteEnabled = rb_depthcontrol.z_write_enable;
  
  auto convert_compare = [](rex::graphics::xenos::CompareFunction func) {
    switch (func) {
      case rex::graphics::xenos::CompareFunction::kNever: return ::plume::RenderComparisonFunction::NEVER;
      case rex::graphics::xenos::CompareFunction::kLess: return ::plume::RenderComparisonFunction::LESS;
      case rex::graphics::xenos::CompareFunction::kEqual: return ::plume::RenderComparisonFunction::EQUAL;
      case rex::graphics::xenos::CompareFunction::kLessEqual: return ::plume::RenderComparisonFunction::LESS_EQUAL;
      case rex::graphics::xenos::CompareFunction::kGreater: return ::plume::RenderComparisonFunction::GREATER;
      case rex::graphics::xenos::CompareFunction::kNotEqual: return ::plume::RenderComparisonFunction::NOT_EQUAL;
      case rex::graphics::xenos::CompareFunction::kGreaterEqual: return ::plume::RenderComparisonFunction::GREATER_EQUAL;
      case rex::graphics::xenos::CompareFunction::kAlways: return ::plume::RenderComparisonFunction::ALWAYS;
      default: return ::plume::RenderComparisonFunction::ALWAYS;
    }
  };
  desc.depthFunction = convert_compare(rb_depthcontrol.zfunc);

  auto convert_blend_op = [](rex::graphics::xenos::BlendOp op) {
    switch (op) {
      case rex::graphics::xenos::BlendOp::kAdd: return ::plume::RenderBlendOperation::ADD;
      case rex::graphics::xenos::BlendOp::kSubtract: return ::plume::RenderBlendOperation::SUBTRACT;
      case rex::graphics::xenos::BlendOp::kMin: return ::plume::RenderBlendOperation::MIN;
      case rex::graphics::xenos::BlendOp::kMax: return ::plume::RenderBlendOperation::MAX;
      case rex::graphics::xenos::BlendOp::kRevSubtract: return ::plume::RenderBlendOperation::REV_SUBTRACT;
      default: return ::plume::RenderBlendOperation::ADD;
    }
  };

  auto convert_blend_factor = [](rex::graphics::xenos::BlendFactor factor) {
    switch (factor) {
      case rex::graphics::xenos::BlendFactor::kZero: return ::plume::RenderBlend::ZERO;
      case rex::graphics::xenos::BlendFactor::kOne: return ::plume::RenderBlend::ONE;
      case rex::graphics::xenos::BlendFactor::kSrcColor: return ::plume::RenderBlend::SRC_COLOR;
      case rex::graphics::xenos::BlendFactor::kOneMinusSrcColor: return ::plume::RenderBlend::INV_SRC_COLOR;
      case rex::graphics::xenos::BlendFactor::kSrcAlpha: return ::plume::RenderBlend::SRC_ALPHA;
      case rex::graphics::xenos::BlendFactor::kOneMinusSrcAlpha: return ::plume::RenderBlend::INV_SRC_ALPHA;
      case rex::graphics::xenos::BlendFactor::kDstColor: return ::plume::RenderBlend::DEST_COLOR;
      case rex::graphics::xenos::BlendFactor::kOneMinusDstColor: return ::plume::RenderBlend::INV_DEST_COLOR;
      case rex::graphics::xenos::BlendFactor::kDstAlpha: return ::plume::RenderBlend::DEST_ALPHA;
      case rex::graphics::xenos::BlendFactor::kOneMinusDstAlpha: return ::plume::RenderBlend::INV_DEST_ALPHA;
      case rex::graphics::xenos::BlendFactor::kConstantColor: return ::plume::RenderBlend::BLEND_FACTOR;
      case rex::graphics::xenos::BlendFactor::kOneMinusConstantColor: return ::plume::RenderBlend::INV_BLEND_FACTOR;
      case rex::graphics::xenos::BlendFactor::kConstantAlpha: return ::plume::RenderBlend::BLEND_FACTOR;
      case rex::graphics::xenos::BlendFactor::kOneMinusConstantAlpha: return ::plume::RenderBlend::INV_BLEND_FACTOR;
      case rex::graphics::xenos::BlendFactor::kSrcAlphaSaturate: return ::plume::RenderBlend::SRC_ALPHA_SAT;
      default: return ::plume::RenderBlend::ONE;
    }
  };

  auto convert_stencil_op = [](rex::graphics::xenos::StencilOp op) {
    switch (op) {
      case rex::graphics::xenos::StencilOp::kKeep: return ::plume::RenderStencilOp::KEEP;
      case rex::graphics::xenos::StencilOp::kZero: return ::plume::RenderStencilOp::ZERO;
      case rex::graphics::xenos::StencilOp::kReplace: return ::plume::RenderStencilOp::REPLACE;
      case rex::graphics::xenos::StencilOp::kIncrementClamp: return ::plume::RenderStencilOp::INCREMENT_AND_CLAMP;
      case rex::graphics::xenos::StencilOp::kDecrementClamp: return ::plume::RenderStencilOp::DECREMENT_AND_CLAMP;
      case rex::graphics::xenos::StencilOp::kInvert: return ::plume::RenderStencilOp::INVERT;
      case rex::graphics::xenos::StencilOp::kIncrementWrap: return ::plume::RenderStencilOp::INCREMENT_AND_WRAP;
      case rex::graphics::xenos::StencilOp::kDecrementWrap: return ::plume::RenderStencilOp::DECREMENT_AND_WRAP;
      default: return ::plume::RenderStencilOp::KEEP;
    }
  };

  desc.alphaToCoverageEnabled = rb_colorcontrol.alpha_to_mask_enable;

  desc.renderTargetBlend[0].blendEnabled = (rb_blendcontrol.color_srcblend != rex::graphics::xenos::BlendFactor::kOne ||
                                            rb_blendcontrol.color_destblend != rex::graphics::xenos::BlendFactor::kZero ||
                                            rb_blendcontrol.alpha_srcblend != rex::graphics::xenos::BlendFactor::kOne ||
                                            rb_blendcontrol.alpha_destblend != rex::graphics::xenos::BlendFactor::kZero);
  desc.renderTargetBlend[0].srcBlend = convert_blend_factor(rb_blendcontrol.color_srcblend);
  desc.renderTargetBlend[0].dstBlend = convert_blend_factor(rb_blendcontrol.color_destblend);
  desc.renderTargetBlend[0].blendOp = convert_blend_op(rb_blendcontrol.color_comb_fcn);
  desc.renderTargetBlend[0].srcBlendAlpha = convert_blend_factor(rb_blendcontrol.alpha_srcblend);
  desc.renderTargetBlend[0].dstBlendAlpha = convert_blend_factor(rb_blendcontrol.alpha_destblend);
  desc.renderTargetBlend[0].blendOpAlpha = convert_blend_op(rb_blendcontrol.alpha_comb_fcn);

  uint8_t write_mask = 0;
  if (rb_color_mask.write_red0) write_mask |= static_cast<uint8_t>(::plume::RenderColorWriteEnable::RED);
  if (rb_color_mask.write_green0) write_mask |= static_cast<uint8_t>(::plume::RenderColorWriteEnable::GREEN);
  if (rb_color_mask.write_blue0) write_mask |= static_cast<uint8_t>(::plume::RenderColorWriteEnable::BLUE);
  if (rb_color_mask.write_alpha0) write_mask |= static_cast<uint8_t>(::plume::RenderColorWriteEnable::ALPHA);
  desc.renderTargetBlend[0].renderTargetWriteMask = write_mask;

  desc.stencilEnabled = rb_depthcontrol.stencil_enable;
  if (desc.stencilEnabled) {
    desc.stencilFrontFace.compareFunction = convert_compare(rb_depthcontrol.stencilfunc);
    desc.stencilFrontFace.passOp = convert_stencil_op(rb_depthcontrol.stencilzpass);
    desc.stencilFrontFace.failOp = convert_stencil_op(rb_depthcontrol.stencilfail);
    desc.stencilFrontFace.depthFailOp = convert_stencil_op(rb_depthcontrol.stencilzfail);
    
    desc.stencilBackFace.compareFunction = convert_compare(rb_depthcontrol.stencilfunc_bf);
    desc.stencilBackFace.passOp = convert_stencil_op(rb_depthcontrol.stencilzpass_bf);
    desc.stencilBackFace.failOp = convert_stencil_op(rb_depthcontrol.stencilfail_bf);
    desc.stencilBackFace.depthFailOp = convert_stencil_op(rb_depthcontrol.stencilzfail_bf);

    desc.stencilReadMask = rb_stencilrefmask.stencilmask;
    desc.stencilWriteMask = rb_stencilrefmask.stencilwritemask;
    desc.stencilReference = rb_stencilrefmask.stencilref;
  }
  
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = color_format;
  desc.depthTargetFormat = ::plume::RenderFormat::D32_FLOAT_S8_UINT;
  
  auto pipeline = plume_device_->createGraphicsPipeline(desc);
  if (!pipeline) {
    REXLOG_ERROR("PlumeCommandProcessor: createGraphicsPipeline failed!");
    return {};
  }
  auto* ptr = pipeline.get();
  graphics_pipelines_[hash] = PipelineEntry{ std::move(pipeline), layout };
  return { ptr, layout };
}

}  // namespace rex::graphics_plume
