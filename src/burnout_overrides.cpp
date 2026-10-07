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
#include "runtime/ps2_rom_device.h"

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {
constexpr const char* kElfName = "SLES_546.27";
constexpr uint32_t kEntry = 0x00100008;
constexpr uint32_t kElfCrc32 = 0xC804E2C1;

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
    // libsif commands. sceSifInitCmd is bound, so the guest command tables,
    // its receive buffer and the IOP buffer address are never set up: every
    // entry point that uses them goes to the runtime, which delivers commands
    // to the IOP handlers and IOP replies to the handlers registered here.
    {0x003AFB58, "sceSifExitCmd"},
    {0x003AFB90, "sceSifSetCmdBuffer"},
    {0x003AFBA8, "sceSifAddCmdHandler"},
    {0x003AFC20, "sceSifRemoveCmdHandler"},
    {0x003AFDA8, "sceSifSendCmd"},
    // isceSifSendCmd: same arguments; the runtime's SIF transfers complete
    // synchronously, so the interrupt-context variant is the same call.
    {0x003AFDE8, "sceSifSendCmd"},
    {0x003AF8B0, "sceSifGetSreg"},

    // loadfile (SID 0x80000006): wrapper of _SifLoadModule(path, argc, argv, &res, 0)
    {0x003B2180, "sceSifLoadModule"},
    {0x003B1EA8, "sceSifSearchModuleByName"}, // rpc 9, send {name[252]}, reply {module id}

    // iopheap (SID 0x80000003)
    {0x003B1B48, "sceSifInitIopHeap"},
    {0x003B1BD0, "sceSifAllocSysMemory"}, // rpc 4, send {size, mode, addr}
    {0x003B1C50, "sceSifFreeIopHeap"},    // rpc 2, send {addr}

    // fileio (SID 0x80000001, served by FILEIO from IOPRP300.IMG, which the
    // emulated IOP does not run). Its only callers, 0x379908 and 0x38AFE0,
    // read rom0:ROMVER; the second is reached at boot through
    // main 0x21B3F8 -> 0x207820 -> 0x1B48F8 -> 0x1E6EB8 -> 0x38B0F8 -> 0x38AFE0.
    {0x003B1468, "sceOpen"},  // uses SceStdioOpenSema, called as (path, 1)
    {0x003B16F8, "sceClose"}, // uses SceStdioCloseSema
    {0x003B1878, "sceRead"},  // uses SceStdioReadSema, called as (fd, buf, n)

    // libcdvd (cdvdfsv SIDs 0x80000592 init, 0x80000593 S-cmd, 0x8000059A/C disk ready)
    {0x00377CA0, "sceCdInit"},
    {0x00378458, "sceCdGetDiskType"}, // S-cmd 3, -1 mapped to 0
    {0x00378480, "sceCdMmode"},       // S-cmd 0x22
    {0x003781A0, "sceCdDiskReady"},   // falls back to the 0x8000059A variant at 0x377F88
    {0x00378558, "sceCdReadClock"},   // S-cmd 1, 16-byte reply, callers decode BCD
    // Internal entry points, only called from the functions above. Bound too
    // so that no path can reach the guest S-cmd and DiskReady bind loops.
    {0x00377F88, "sceCdDiskReady"},   // old-protocol DiskReady (SID 0x8000059A)
    {0x003783C0, "sceCdGetDiskType"}, // S-cmd 3 core, without the -1 -> 0 mapping
    {0x00377AD8, "sceCdSyncS"},       // polls sceSifCheckStatRpc on the S-cmd client
};
// clang-format on

// rom0:ROMVER as a European console stores it: "VVVVRTYYYYMMDD", '\n' and a
// NUL, 16 bytes in the ROMDIR. The runtime's default file holds only the 14
// visible characters. 0x379908 reads the file one byte at a time up to the NUL
// and parses the 9 bytes before it as a date (> 20010608), so a missing
// terminator makes it read past the end of the file.
[[maybe_unused]] const bool kRomProfileRegistered = [] {
    PS2RomProfile profile;
    profile.id = "burnout-dominator-pal";
    profile.provider = "burnout";
    profile.matcher = {kElfName, kEntry, kElfCrc32};
    constexpr char kRomVersion[] = "0200EC20040614\n";
    static_assert(sizeof(kRomVersion) == 16);
    profile.files["ROMVER"] = std::vector<uint8_t>(kRomVersion, kRomVersion + sizeof(kRomVersion));
    PS2RomDevice::registerProfile(std::move(profile));
    return true;
}();

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

PS2_REGISTER_GAME_OVERRIDE("Burnout Dominator PAL", kElfName, kEntry, kElfCrc32, apply)
