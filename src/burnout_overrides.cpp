// Burnout Dominator PAL (SLES_546.27) runtime bindings.
//
// The ELF is stripped. Sony SDK library functions statically linked into it are
// identified by address (see docs/SDK_FUNCTIONS.md for the evidence of each one)
// and routed to the runtime's implementations of the same SDK calls, which talk
// to the emulated IOP, the original disc image and the host devices.
//
// Only functions whose identity is established are listed. Game code is never
// replaced here, and no binding returns a fabricated success.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <iostream>

namespace {
struct Binding {
    uint32_t address;
    const char* handler;
};

// clang-format off
constexpr Binding kBindings[] = {
    // libsif (EE side of the SIF RPC/command protocol)
    {0x003AF8D8, "sceSifInitCmd"},
    {0x003B0020, "sceSifInitRpc"},
    {0x003B0668, "sceSifBindRpc"},
    {0x003B0848, "sceSifCallRpc"},
    {0x003B2320, "sceSifSyncIop"},
    {0x003B2370, "sceSifRebootIop"},
};
// clang-format on

void apply(PS2Runtime& runtime) {
    unsigned bound = 0;
    for (const Binding& binding : kBindings) {
        if (ps2_game_overrides::bindAddressHandler(runtime, binding.address, binding.handler))
            ++bound;
        else
            std::cerr << "[BDR] could not bind " << binding.handler << " at 0x" << std::hex
                      << binding.address << std::dec << '\n';
    }
    std::cout << "[BDR] bound " << bound << '/' << std::size(kBindings) << " SDK functions\n";
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("Burnout Dominator PAL", "SLES_546.27", 0x00100008, 0xC804E2C1, apply)
