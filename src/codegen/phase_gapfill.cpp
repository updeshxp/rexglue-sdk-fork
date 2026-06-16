/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <unordered_set>

#include <rex/codegen/function_scanner.h>
#include <rex/codegen/phases.h>
#include "decoded_binary.h"
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Split a code region into function segments based on terminators (blr, tail calls).
std::vector<CodeRegion> splitRegionOnTerminators(
    const CodeRegion& region, const BinaryView& binary, DecodedBinary& decodedBinary,
    const std::unordered_set<uint32_t>& knownCallables) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = region.start;

  // Track the furthest forward target seen from conditional branches within
  // the current segment. A conditional branch (bc/beq/bne/…) always has a
  // fall-through, so its target is guaranteed to be inside the same function.
  // We must not split a segment on a terminator (blr, bctr, b) while a
  // conditional branch is still pointing to code ahead of it — that would
  // fragment a real function in a gap region into bogus pieces.
  uint32_t maxConditionalForwardTarget = region.start;

  for (uint32_t addr = region.start; addr < region.end; addr += 4) {
    const uint8_t* data = binary.translate(addr);
    if (!data)
      break;

    uint32_t raw = load_and_swap<uint32_t>(data);
    auto decoded = decode_instruction(addr, raw);
    bool shouldSplit = false;
    const char* reason = nullptr;

    // Extend the forward-span window on conditional branches. These unambiguously
    // stay inside the same function (they have a fall-through path), so any
    // terminator before their target must not end the segment.
    if (decoded.is_conditional() && decoded.branch_target.has_value()) {
      uint32_t target = decoded.branch_target.value();
      if (target > addr && target < region.end) {
        maxConditionalForwardTarget = std::max(maxConditionalForwardTarget, target);
      }
    }

    // Only treat a terminator as a segment boundary once we are past all
    // forward conditional-branch targets in this segment. Before that point
    // the terminator is inside a function body, not at its end.
    bool pastForwardSpan = (addr >= maxConditionalForwardTarget);

    // Check for terminators
    if (decoded.is_return()) {
      if (pastForwardSpan) {
        shouldSplit = true;
        reason = "blr";
      }
    } else if (decoded.opcode == Opcode::bcctr && !decoded.is_conditional()) {
      // Unconditional `bctr` is either:
      //  (a) a vtable-forwarder thunk tail-dispatch (plain lwz → mtctr → bctr, no lis
      //      table base) → should split so each thunk is a separate callable.
      //  (b) a switch/jump-table dispatcher (indexed lwzx/lbzx + lis table → mtctr →
      //      bctr, with the jump table data and case bodies immediately following) →
      //      must NOT split: the table bytes + case bodies belong to the same function,
      //      and splitting here registers the table as a bogus gap function that
      //      overlaps the real function, causing internal labels to fail classifyTarget
      //      and emit REX_FATAL "Unresolved conditional branch".
      //
      // Use detectJumpTable() — the same detector the recompiler uses — to distinguish
      // them. It returns a table (with .targets) only for switch dispatchers; for vtable
      // thunks (no indexed load, no lis) it returns nullopt.
      if (auto jt = detectJumpTable(decodedBinary, addr, region, region.start, region.end)) {
        // Switch dispatch: extend the forward-span window to cover all case-body
        // targets so no terminator inside the table or case bodies splits the segment.
        for (uint32_t t : jt->targets) {
          if (t > addr && t < region.end) {
            maxConditionalForwardTarget = std::max(maxConditionalForwardTarget, t);
          }
        }
        REXCODEGEN_TRACE(
            "GapFill: bctr at 0x{:08X} is switch dispatch (table=0x{:08X}, {} cases) — "
            "no split, extended span to 0x{:08X}",
            addr, jt->tableAddress, jt->targets.size(), maxConditionalForwardTarget);
        // Do not set shouldSplit — the switch and its case bodies stay in one segment.
      } else if (pastForwardSpan) {
        // Vtable thunk or other non-switch bctr: split so each thunk is registered.
        shouldSplit = true;
        reason = "bctr";
      }
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      uint32_t target = decoded.branch_target.value();
      // An unconditional `b` never falls through, so the following instruction
      // begins a new segment unless this is a backward branch into the segment
      // we are currently accumulating (an intra-function loop). A forward or
      // far branch is a tail call / function exit. Previously we only split when
      // the target was an already-known callable, which missed tail calls to
      // not-yet-discovered functions: the segment then swallowed the function
      // that physically follows the branch, and block discovery (which stops at
      // the branch) left that following function unregistered -> runtime trap
      // "Call to invalid or unregistered function".
      bool loopsBack = (target >= segmentStart && target <= addr);
      if (pastForwardSpan && (!loopsBack || knownCallables.contains(target))) {
        shouldSplit = true;
        reason = "tail branch";
      }
    }

    if (shouldSplit) {
      uint32_t segmentEnd = addr + 4;
      if (segmentEnd > segmentStart) {
        segments.push_back({segmentStart, segmentEnd});
        REXCODEGEN_TRACE("GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})", segmentStart,
                         segmentEnd, reason, addr);
      }
      segmentStart = segmentEnd;
      // Reset the forward-span window for the new segment.
      maxConditionalForwardTarget = segmentStart;
    }
  }

  // Handle remaining code after last terminator
  if (segmentStart < region.end) {
    segments.push_back({segmentStart, region.end});
  }

  return segments;
}

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& decodedBinary = ctx.decoded();
  auto& scan = ctx.scan;

  // Build set of known callables for tail call detection
  std::unordered_set<uint32_t> knownCallables;
  for (const auto& [addr, node] : graph.functions()) {
    knownCallables.insert(addr);
  }

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  for (const auto& region : scan.codeRegions) {
    // Split region on terminators (blr, tail calls), then check each segment
    auto segments = splitRegionOnTerminators(region, binary, decodedBinary, knownCallables);

    for (const auto& segment : segments) {
      // Skip if this segment's start is already a registered function entry
      if (graph.isEntryPoint(segment.start))
        continue;

      // Skip if this segment's start is inside another function
      if (auto* containingFunc = graph.getFunctionContaining(segment.start)) {
        continue;
      }

      // Skip if this looks like exception handler data (handler ptr + rdata ptr)
      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;

      uint32_t segmentSize = segment.size();
      graph.addFunction(segment.start, segmentSize, FunctionAuthority::GAP_FILL, false);

      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                       segment.start, segment.start, segment.end, segmentSize);
      segmentsCreated++;
    }

    gapsFound++;
  }

  if (segmentsCreated > 0) {
    REXCODEGEN_TRACE("Analyze: registered {} gap functions from {} regions", segmentsCreated,
                     gapsFound);
  } else {
    REXCODEGEN_TRACE("Analyze: no uncovered regions found");
  }
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      // This GAP_FILL is inside another function's blocks
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        // Absorbed by higher authority - remove
        toRemove.push_back(addr);
        break;
      } else if (otherAddr < addr) {
        // Both GAP_FILL, other has lower address - it survives
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  // Discover blocks for gap-filled functions
  auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
  size_t discovered = discoverPendingFunctions(ctx, known);
  REXCODEGEN_TRACE("Analyze: discovered blocks for {} gap-filled functions", discovered);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
