// lib/sweep_compression/CompressionTables.h
//
// Verbatim port of the three sin lookup tables from References/globals.h
// lines 80-120. Each table is one cycle of |sin(theta)| sampled at 1° per
// entry, scaled to amplitude 100. The Ardu-Stim compression simulator
// modulates the base RPM by a fraction of these tables (see
// References/ardustim.ino:333-389 and SweepCompression.cpp).
//
// Sizes (line-count verified against the reference):
//   sin_100_180[180]
//   sin_100_120[120]
//   sin_100_90 [ 90]
//
// All three live in `.rodata` (constexpr) — the ESP32 instruction cache
// covers them with zero jitter overhead at compression-task rates.
//
// Owner: Agent C (sweep-store). Parent reference: implementation_plan.md §M4.2.

#pragma once

#include <stdint.h>

namespace SweepCompressionTables {

// A sin wave of amplitude 100 with a complete cycle in 180 degrees
// (1 entry per degree). 180 entries. Copied verbatim from
// References/globals.h:80-95.
constexpr uint8_t sin_100_180[180] = {
    0,0,0,0,0,1,1,1,2,2,3,4,4,5,6,7,8,9,10,11,
    12,13,14,15,17,18,19,21,22,24,25,27,28,30,
    31,33,35,36,38,40,41,43,45,47,48,50,52,53,
    55,57,59,60,62,64,65,67,69,70,72,73,75,76,
    78,79,81,82,83,85,86,87,88,89,90,91,92,93,
    94,95,96,96,97,98,98,99,99,99,100,100,100,
    100,100,100,100,100,100,99,99,99,98,98,97,
    96,96,95,94,93,92,91,90,89,88,87,86,85,83,
    82,81,79,78,76,75,73,72,70,69,67,65,64,62,
    60,59,57,55,53,52,50,48,47,45,43,41,40,38,
    36,35,33,31,30,28,27,25,24,22,21,19,18,17,
    15,14,13,12,11,10,9,8,7,6,5,4,4,3,2,2,1,1,
    1,0,0,0,0
};

// A sin wave of amplitude 100 with a complete cycle in 90 degrees
// (1 entry per degree). 90 entries. Copied verbatim from
// References/globals.h:98-106.
constexpr uint8_t sin_100_90[90] = {
    0,0,0,1,2,3,4,6,8,10,12,14,17,19,22,25,28,
    31,35,38,41,45,48,52,55,59,62,65,69,72,75,
    78,81,83,86,88,90,92,94,96,97,98,99,100,100,
    100,100,100,99,98,97,96,94,92,90,88,86,83,81,
    78,75,72,69,65,62,59,55,52,48,45,41,38,35,31,
    28,25,22,19,17,14,12,10,8,6,4,3,2,1,0,0
};

// A sin wave of amplitude 100 with a complete cycle in 120 degrees
// (1 entry per degree). 120 entries. Copied verbatim from
// References/globals.h:110-120.
constexpr uint8_t sin_100_120[120] = {
    0,0,0,1,1,2,2,3,4,5,7,8,10,11,13,15,17,19,
    21,23,25,27,30,32,35,37,40,42,45,47,50,53,
    55,58,60,63,65,68,70,73,75,77,79,81,83,85,
    87,89,90,92,93,95,96,97,98,98,99,99,100,100,
    100,100,100,99,99,98,98,97,96,95,93,92,90,89,
    87,85,83,81,79,77,75,73,70,68,65,63,60,58,55,
    53,50,47,45,42,40,37,35,32,30,27,25,23,21,19,
    17,15,13,11,10,8,7,5,4,3,2,2,1,1,0,0
};

}  // namespace SweepCompressionTables
