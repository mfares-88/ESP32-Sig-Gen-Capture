// lib/ui_lvgl/serial_cli.h — M9 (Agent E) dual-mode serial protocol.
//
// Per implementation_plan.md §3 / §6:
//   * Legacy single-byte opcodes from References/comms.cpp:
//       a/c/C/L/n/N/p/P/R/r/s/S/X
//     including the `r` HI-LO big-endian byte gotcha
//     (References/comms.cpp:128-130).
//   * New text protocol — first byte alphabetic + space => text mode:
//       LIST, SELECT <key>, COMPILE <dsl...>, SAVE user/<name>,
//       LOAD user/<name>, DELETE user/<name>, CAPTURE START|STOP,
//       SWEEP <low> <high> <mode> <interval_us>,
//       COMP <on|off> <cyl> <thresh> <peak> <dyn>.
//
// Both modes are non-blocking — serialCliPoll() consumes whatever bytes
// are immediately available and processes complete commands when it has
// them. All side-effects go through the manager queue (gCtrlQ) per
// §6 Agent E hard rules — this TU never touches gGen / gCfg directly.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-shot init. Safe to call after Serial.begin().
void serialCliBegin();

// Pump from loop() — drains pending bytes, dispatches commands. Returns
// the number of complete commands processed this call (for diagnostics).
int  serialCliPoll();

#ifdef __cplusplus
}
#endif
