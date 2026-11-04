#ifndef PS2_VU1_STATS_H
#define PS2_VU1_STATS_H

#include <cstdint>
#include <string>

// Work and host time of each VU1 microprogram, to choose which ones to replace
// with native code. Off unless PS2_VU_PROGRAM_STATS is set (or setEnabled).
namespace ps2_vu_stats
{
bool enabled();
void setEnabled(bool on);
void reset();
// One line per program, the most expensive first: its microcode image (FNV-1a
// of the 16 KiB, as the vu1-<hash>.code recordings are named), entry address and
// compiled routine (the hash in its generated name), which a program keeps
// whatever else its image holds.
std::string report();
// The hash in the name of the compiled routine the program at `pc` runs, or 0.
uint64_t routineName(const uint8_t *code, uint32_t codeSize, uint32_t pc);
}

#endif
