// Checks src/burnout_overrides.cpp without the generated guest code: every
// SDK binding resolves to the intended runtime handler, and rom0:ROMVER has the
// layout the game's parser expects.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr const char* kElfName = "SLES_546.27";
constexpr uint32_t kEntry = 0x00100008;
constexpr uint32_t kElfCrc32 = 0xC804E2C1;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

struct Expected {
    uint32_t address;
    PS2Runtime::RecompiledFunction handler;
    const char* name;
};

// A second, independent record of src/burnout_overrides.cpp: a renamed or
// mistyped handler, or a name that resolves to another handler, fails here.
const Expected kExpected[] = {
    {0x003AF8D8, &ps2_stubs::sceSifInitCmd, "sceSifInitCmd"},
    {0x003B0020, &ps2_stubs::sceSifInitRpc, "sceSifInitRpc"},
    {0x003B0668, &ps2_stubs::sceSifBindRpc, "sceSifBindRpc"},
    {0x003B0848, &ps2_syscalls::sceSifCallRpc, "sceSifCallRpc"},
    {0x003B2320, &ps2_stubs::sceSifSyncIop, "sceSifSyncIop"},
    {0x003B2370, &ps2_stubs::sceSifRebootIop, "sceSifRebootIop"},
    {0x003B2180, &ps2_syscalls::sceSifLoadModule, "sceSifLoadModule"},
    {0x003B1B48, &ps2_stubs::sceSifInitIopHeap, "sceSifInitIopHeap"},
    {0x003B1BD0, &ps2_stubs::sceSifAllocSysMemory, "sceSifAllocSysMemory"},
    {0x003B1C50, &ps2_stubs::sceSifFreeIopHeap, "sceSifFreeIopHeap"},
    {0x003B1468, &ps2_stubs::sceOpen, "sceOpen"},
    {0x003B16F8, &ps2_stubs::sceClose, "sceClose"},
    {0x003B1878, &ps2_stubs::sceRead, "sceRead"},
    {0x00377CA0, &ps2_stubs::sceCdInit, "sceCdInit"},
    {0x00378458, &ps2_stubs::sceCdGetDiskType, "sceCdGetDiskType"},
    {0x00378480, &ps2_stubs::sceCdMmode, "sceCdMmode"},
    {0x003781A0, &ps2_stubs::sceCdDiskReady, "sceCdDiskReady"},
    {0x00378558, &ps2_stubs::sceCdReadClock, "sceCdReadClock"},
    {0x00377F88, &ps2_stubs::sceCdDiskReady, "sceCdDiskReady (old protocol)"},
    {0x003783C0, &ps2_stubs::sceCdGetDiskType, "sceCdGetDiskType (core)"},
    {0x00377AD8, &ps2_stubs::sceCdSyncS, "sceCdSyncS"},
};

void setReg(R5900Context& ctx, int reg, uint32_t value) {
    ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

int32_t returnValue(const R5900Context& ctx) {
    return static_cast<int32_t>(getRegU32(&ctx, 2));
}

void testBindings() {
    PS2Runtime runtime;
    ps2_game_overrides::applyMatching(runtime, std::string("disc/") + kElfName, kEntry, kElfCrc32, true);
    std::size_t bound = 0;
    for (std::size_t slot = 0x00100000 >> 2; slot < (0x003BEB28 >> 2); ++slot)
        bound += g_ps2RecompiledFunctionTable[slot] != nullptr;
    check(bound == std::size(kExpected),
          "bound " + std::to_string(bound) + " .text slots, expected " + std::to_string(std::size(kExpected)));
    for (const Expected& expected : kExpected) {
        check(g_ps2RecompiledFunctionTable[expected.address >> 2] == expected.handler,
              std::string(expected.name) + " is not bound to its runtime handler");
    }
}

void testRomVersion() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bdr-overrides-test";
    std::filesystem::create_directories(root);
    PS2Runtime::IoPaths paths;
    paths.elfDirectory = paths.hostRoot = paths.cdRoot = paths.mcRoot = root;
    PS2Runtime::setIoPaths(paths);

    PS2Runtime runtime;
    std::string error;
    check(runtime.romDevice().configure({kElfName, kEntry, kElfCrc32}, &error), "ROM0 configure: " + error);
    check(runtime.romDevice().activeProfile() == "burnout-dominator-pal", "Burnout ROM0 profile not selected");

    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0xA5);
    constexpr uint32_t kPath = 0x1000, kBuffer = 0x2000;
    std::memcpy(rdram.data() + kPath, "rom0:ROMVER", 12);
    R5900Context ctx{};
    setReg(ctx, 4, kPath);
    setReg(ctx, 5, PS2_FIO_O_RDONLY);
    ps2_stubs::sceOpen(rdram.data(), &ctx, &runtime);
    const int32_t fd = returnValue(ctx);
    check(fd >= 0, "sceOpen(rom0:ROMVER) failed");

    // Same access pattern as 0x379908: one byte per sceRead, up to the NUL.
    uint32_t length = 0;
    for (; length < 256; ++length) {
        setReg(ctx, 4, static_cast<uint32_t>(fd));
        setReg(ctx, 5, kBuffer + length);
        setReg(ctx, 6, 1);
        ps2_stubs::sceRead(rdram.data(), &ctx, &runtime);
        if (returnValue(ctx) != 1) break;
        if (rdram[kBuffer + length] == 0) break;
    }
    check(length == 15, "ROMVER NUL at offset " + std::to_string(length) + ", expected 15");
    check(std::memcmp(rdram.data() + kBuffer, "0200EC20040614\n", 16) == 0, "ROMVER bytes differ");
    // 0x379908 converts the 9 bytes before the NUL and compares with 20010608.
    const long date = std::strtol(reinterpret_cast<const char*>(rdram.data() + kBuffer + length - 9), nullptr, 10);
    check(date == 20040614, "ROMVER date parsed as " + std::to_string(date));

    setReg(ctx, 4, static_cast<uint32_t>(fd));
    ps2_stubs::sceClose(rdram.data(), &ctx, &runtime);
    check(returnValue(ctx) == 0, "sceClose(rom0:ROMVER) failed");

    PS2Runtime other;
    check(other.romDevice().configure({"SLUS_000.00", kEntry, 0}, &error) && other.romDevice().activeProfile().empty(),
          "the Burnout ROM0 profile must not apply to other ELFs");
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    testBindings();
    testRomVersion();
    if (g_failures) {
        std::cerr << g_failures << " Burnout override check(s) failed\n";
        return 1;
    }
    std::cout << "Burnout override tests passed (" << std::size(kExpected) << " bindings, ROM0 profile)\n";
    return 0;
}
