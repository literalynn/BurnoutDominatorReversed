#ifndef PS2_VU1_NATIVE_H
#define PS2_VU1_NATIVE_H

// Native VU programs: C++ translations of whole VU1 and VU0 microprograms, run from the MSCAL,
// MSCNT or VCALLMS that starts one to its E bit without stepping through the microcode pair by
// pair. tools/translate_vu_programs.py writes them from the game's own microcode; this header
// holds the registry, ps2_vu1_native_ops.h the VU arithmetic on host SSE registers.
//
// PS2_VU_NATIVE selects them at run time: unset or 1, the translations run; 0, off, the
// microcode runs; "shadow", the microcode still runs while each translation runs on copies of
// its inputs and the two results are compared (GIF packets, VU data memory, registers).

#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

// CMake defines PS2X_VU_NATIVE_X86_64 on x86-64, where it compiles the translations for SSE4.1
// and FMA3; find() finds none on a CPU without them.
#if defined(PS2X_VU_NATIVE_X86_64) && (defined(_M_X64) || defined(__x86_64__))
#define PS2_VU_NATIVE_AVAILABLE 1
#else
#define PS2_VU_NATIVE_AVAILABLE 0
#endif

namespace ps2_vu_native
{
// What a translation reads and changes. The registers it does not model (status, P, R) keep
// their values; GIF packets go to kick in the order the microcode kicks them.
struct Context
{
    uint8_t *data; // VU data memory, 16 KiB on VU1, 4 KiB on VU0
    VU1State *state;
    void (*kick)(void *user, const uint8_t *packet, uint32_t bytes);
    void *user;
    uint64_t cycles; // set by the translation: the VU1 cycles the microcode takes, pipeline flush included
};

struct Program
{
    const char *name;
    uint32_t pc;         // start address
    uint32_t begin, end; // microcode bytes the translation was made from
    uint64_t hash;       // their FNV-1a 64
    const uint8_t *code; // the bytes themselves, compared at each new microcode generation
    bool (*run)(Context &);
    uint32_t unit; // 0: VU0, 1: VU1
};

// Called by the translations' static initializers.
void add(const Program &program);

enum class Mode
{
    Off,
    Shadow,
    On
};
Mode mode();
void setMode(Mode mode);

// The translation for the program a start at pc runs on unit (0: VU0, 1: VU1), if there is
// one: the code is checked once per microcode generation. Each unit calls it from one thread.
// None on a CPU without SSE4.1 and FMA3.
const Program *find(const uint8_t *code, uint32_t codeSize, uint32_t pc, uint64_t generation, uint32_t unit);

// Runs a translation as VU1Interpreter::run runs microcode: rounding toward zero, denormal
// operands read as zero, results below FLT_MIN flushed to zeros of their sign. false: the start
// holds a value the program jumps through that the translation does not have (an address it
// left in a register or in VU1 memory), nothing has changed and the microcode must run.
bool run(const Program &program, Context &context);

// Counts of shadow comparisons, one line per program; empty when nothing was compared.
std::string report();
// All programs together.
void totals(uint64_t &compared, uint64_t &identical);

// Shadow mode, called by VU1Interpreter, one comparison at a time per unit: start() runs the
// translation on copies of the inputs, kick() collects what the microcode kicks meanwhile and
// finish() compares the two when the microcode reaches its E bit, cycles after it started.
// cancel(): the microcode did not end within its start's cycles, which the translation does
// not model.
void shadowStart(const Program &program, const uint8_t *data, const VU1State &state);
bool shadowActive(uint32_t unit);
void shadowKick(const uint8_t *packet, uint32_t bytes);
void shadowFinish(uint32_t unit, const uint8_t *data, const VU1State &state, uint64_t cycles);
void shadowCancel(uint32_t unit);

// What a kick of VU1 data memory at a qword address sends, as XGKICK reads it: GIF tags until
// the one with EOP, wrapping at the end of memory, into out (capacity bytes, as the
// interpreter's PATH1 buffer). qwords counts what PATH1 reads, one qword every two cycles; a
// packet that does not fit is dropped, as the interpreter drops it, after the tag that
// overflows.
struct GifPacket
{
    uint32_t bytes;  // 0 when dropped
    uint32_t qwords;
};
GifPacket gifPacket(const uint8_t *data, uint32_t qwordAddress, uint8_t *out, uint32_t capacity);

} // namespace ps2_vu_native

#endif
