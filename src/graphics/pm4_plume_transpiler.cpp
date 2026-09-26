#include <rex/graphics/pm4_plume_transpiler.h>

namespace rex::graphics {

Pm4PlumeTranspiler::PacketKind Pm4PlumeTranspiler::ObservePacket(uint32_t opcode,
                                                                  uint32_t count) {
  if (!enabled_) {
    return PacketKind::kOther;
  }
  ++stats_.packets_seen;
  PacketKind kind = PacketKind::kOther;
  switch (opcode) {
    case 0x25:  // PM4_SET_STATE
    case 0x2D:  // PM4_SET_CONSTANT
    case 0x55:  // PM4_SET_CONSTANT2
    case 0x56:  // PM4_SET_SHADER_CONSTANTS
    case 0x2F:  // PM4_LOAD_ALU_CONSTANT
      kind = PacketKind::kSetState;
      ++stats_.state_packets;
      break;
    case 0x27:  // PM4_IM_LOAD
    case 0x2B:  // PM4_IM_LOAD_IMMEDIATE
      kind = PacketKind::kSetShader;
      ++stats_.shader_packets;
      break;
    case 0x22:  // PM4_DRAW_INDX
    case 0x36:  // PM4_DRAW_INDX_2
      kind = PacketKind::kDraw;
      ++stats_.draw_packets;
      break;
    case 0x64:  // PM4_XE_SWAP
      kind = PacketKind::kSwap;
      ++stats_.swap_packets;
      break;
    default:
      ++stats_.fallback_packets;
      break;
  }
  (void)count;
  last_packet_kind_ = kind;
  return last_packet_kind_;
}

void Pm4PlumeTranspiler::ObserveRegisterWrite(uint32_t index, uint32_t value) {
  if (!enabled_) return;
  last_register_index_ = index;
  last_register_value_ = value;
}

void Pm4PlumeTranspiler::ObserveShaderLoad(uint32_t shader_type, uint32_t guest_address,
                                           uint32_t dword_count) {
  if (!enabled_) return;
  (void)shader_type;
  last_shader_address_ = guest_address;
  last_shader_dwords_ = dword_count;
}

void Pm4PlumeTranspiler::ObserveDraw(std::string_view opcode_name, uint32_t primitive_type,
                                     uint32_t index_count, bool indexed) {
  if (!enabled_) return;
  (void)opcode_name;
  (void)primitive_type;
  (void)indexed;
  last_draw_indices_ = index_count;
}

}  // namespace rex::graphics
