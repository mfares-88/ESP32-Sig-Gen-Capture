// PatternRef — universal pattern handle for the byte-packed multi-channel
// signal-generator backend. See implementation_plan.md §3.1 (interface
// contract authored by Agent A, consumed by Agents B/D/E).
//
// Byte encoding (per References/CLAUDE.md and References/ardustim.ino:260-283):
//   bit0 = crank channel state
//   bit1 = cam1 channel state
//   bit2 = cam2 channel state
//   bit3 = knock channel state (reserved — ISR ignores)
//   bits 4..7 = reserved (must be zero)
//
// Lifetime / placement:
//   - Builtin tables live in .rodata (flash). Xtensa I/D-cache caches them.
//   - User/DSL-compiled tables live in PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM)
//     and are freed via dslFree() / PatternLibrary::unregisterUser().
//   - `name_key` MUST point to a string with static storage duration
//     (.rodata literal or an interned strdup that outlives the registration).
//
// Atomicity note (M0.1 documentation, used from M4 onward):
//   Naturally aligned 32-bit reads on Xtensa are atomic — no critical section
//   is needed when the consumer reads a single volatile uint32_t published
//   by the ISR. PatternRef itself is immutable once published.

#pragma once

#include <stdint.h>

struct PatternRef {
  const uint8_t* table;        // bit0=crank, bit1=cam1, bit2=cam2, bit3=knock(rsvd)
  uint16_t       slot_count;   // length of table[]
  uint16_t       degrees;      // 360 or 720
  float          rpm_scaler;   // slot_count / 120.0  (Ardu-Stim convention)
  uint8_t        channel_mask; // bits set = channels actually used by this pattern
  const char*    name_key;     // stable string key in .rodata, used for NVS persistence
};
