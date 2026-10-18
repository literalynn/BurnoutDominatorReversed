#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/host_settings.h"
#include "runtime/host_audio.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <cstring>
#include <chrono>
#include <cmath>

using namespace ps2_syscalls;

namespace
{
    // Guest memory address ranges for test data
    constexpr uint32_t GUEST_STRING_AREA_START = 0x1000;
    constexpr uint32_t GUEST_BUFFER_AREA_START = 0x2000;
    constexpr uint32_t GUEST_STACK_AREA_START = 0x6000;
    constexpr uint32_t GUEST_MC_SYNC_CMD_ADDR = GUEST_BUFFER_AREA_START + 0x1C00;
    constexpr uint32_t GUEST_MC_SYNC_RESULT_ADDR = GUEST_BUFFER_AREA_START + 0x1C04;
    constexpr uint32_t GUEST_MC_TABLE_ADDR = GUEST_BUFFER_AREA_START + 0x2000;
    
    // Common file I/O flag combinations
    constexpr uint32_t PS2_FIO_WRITE_CREATE_TRUNC = 
        PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC;

    struct SceMcStDateTime
    {
        uint8_t resv2;
        uint8_t sec;
        uint8_t min;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint16_t year;
    };

    struct SceMcTblGetDir
    {
        SceMcStDateTime create;
        SceMcStDateTime modify;
        uint32_t fileSizeByte;
        uint16_t attrFile;
        uint16_t reserve1;
        uint32_t reserve2;
        uint32_t pdaAplNo;
        char entryName[32];
    };

    static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

    struct GuestIoStat
    {
        uint32_t mode;
        uint32_t attr;
        uint32_t size;
        uint8_t ctime[8];
        uint8_t atime[8];
        uint8_t mtime[8];
        uint32_t hisize;
    };

    static_assert(sizeof(GuestIoStat) == 40u);

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context *ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(ctx, reg));
    }

    void writeGuestString(uint8_t *rdram, uint32_t addr, const std::string &value)
    {
        std::memcpy(rdram + addr, value.c_str(), value.size() + 1);
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    int32_t readGuestS32(const uint8_t *rdram, uint32_t addr)
    {
        int32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    void clearContext(R5900Context &ctx)
    {
        std::memset(&ctx, 0, sizeof(ctx));
    }

    int32_t syncMc(std::vector<uint8_t> &rdram, int32_t *cmdOut = nullptr)
    {
        R5900Context syncCtx{};
        setRegU32(syncCtx, 4, 0u);
        setRegU32(syncCtx, 5, GUEST_MC_SYNC_CMD_ADDR);
        setRegU32(syncCtx, 6, GUEST_MC_SYNC_RESULT_ADDR);
        ps2_stubs::sceMcSync(rdram.data(), &syncCtx, nullptr);

        if (cmdOut)
        {
            *cmdOut = readGuestS32(rdram.data(), GUEST_MC_SYNC_CMD_ADDR);
        }
        return readGuestS32(rdram.data(), GUEST_MC_SYNC_RESULT_ADDR);
    }

    struct TempPaths
    {
        std::filesystem::path base;
        std::filesystem::path mcRoot;
        std::filesystem::path cdRoot;

        ~TempPaths()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    };

    TempPaths makeTempPaths()
    {
        TempPaths paths;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        paths.base = std::filesystem::temp_directory_path()
                   / ("ps2recomp-mc0-" + std::to_string(now));
        paths.mcRoot = paths.base / "mcroot";
        paths.cdRoot = paths.base / "cdroot";
        std::filesystem::create_directories(paths.mcRoot);
        std::filesystem::create_directories(paths.cdRoot);
        return paths;
    }

    struct TestContext
    {
        TempPaths paths;
        std::vector<uint8_t> rdram;
        R5900Context ctx;
        PS2Runtime runtime;

        TestContext() : paths(makeTempPaths()), rdram(PS2_RAM_SIZE, 0)
        {
            PS2Runtime::IoPaths ioPaths;
            ioPaths.elfDirectory = paths.cdRoot;
            ioPaths.hostRoot = paths.cdRoot;
            ioPaths.cdRoot = paths.cdRoot;
            ioPaths.mcRoot = paths.mcRoot;
            PS2Runtime::setIoPaths(ioPaths);
        }
    };
}

void register_ps2_runtime_io_tests()
{
    MiniTest::Case("PS2RuntimeIO", [](TestCase &tc)
    {
        tc.Run("ROM0 ROMVER is exposed as the 14-byte firmware pseudo-file", [](TestCase &t)
        {
            TestContext test;
            constexpr uint32_t kPathAddr = GUEST_STRING_AREA_START;
            constexpr uint32_t kBufferAddr = GUEST_BUFFER_AREA_START;
            constexpr char kExpectedRomVersion[] = "0200AC20040614";
            static_assert(sizeof(kExpectedRomVersion) - 1u == 14u);

            writeGuestString(test.rdram.data(), kPathAddr, "rom0:ROMVER");
            std::memset(test.rdram.data() + kBufferAddr, 0xA5, 16u);
            setRegU32(test.ctx, 4, kPathAddr);
            setRegU32(test.ctx, 5, PS2_FIO_O_RDONLY);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            const int32_t fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "fioOpen should recognize rom0:ROMVER without a host file");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, kBufferAddr);
            setRegU32(test.ctx, 6, 14u);
            fioRead(test.rdram.data(), &test.ctx, &test.runtime);
            t.Equals(getRegS32(&test.ctx, 2), 14, "fioRead should return the complete ROMVER payload");
            t.IsTrue(std::memcmp(test.rdram.data() + kBufferAddr,
                                 kExpectedRomVersion,
                                 sizeof(kExpectedRomVersion) - 1u) == 0,
                     "ROMVER should use the normal consumer-console format");
            t.Equals(static_cast<uint32_t>(test.rdram[kBufferAddr + 14u]), 0xA5u,
                     "ROMVER reads must not append a terminator");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);
            t.Equals(getRegS32(&test.ctx, 2), 0, "fioClose should release the ROMVER descriptor");
        });

        tc.Run("ROM0 profiles can extend and override files without a BIOS", [](TestCase &t)
        {
            PS2RomProfile profile;
            profile.id = "runtime-io-test";
            profile.provider = "test-extension";
            profile.matcher.elfName = "rom_profile_test.elf";
            profile.files["CONFIG"] = {'p', 'r', 'o', 'f', 'i', 'l', 'e'};
            profile.files["ROMVER"] = {'9', '9', '9', '9', 'T', '2', '0', '2', '6', '0', '8', '2', '4', 'X'};
            PS2RomDevice::registerProfile(std::move(profile));

            TestContext test;
            std::string error;
            t.IsTrue(test.runtime.romDevice().configure({"rom_profile_test.elf", 0u, 0u}, &error),
                     "a uniquely matched ROM0 profile should configure");
            t.Equals(std::string(test.runtime.romDevice().activeProvider()), std::string("test-extension"),
                     "the selected ROM0 profile should expose its provider");

            constexpr uint32_t kPathAddr = GUEST_STRING_AREA_START;
            constexpr uint32_t kBufferAddr = GUEST_BUFFER_AREA_START;
            writeGuestString(test.rdram.data(), kPathAddr, "rom0:CONFIG");
            setRegU32(test.ctx, 4, kPathAddr);
            setRegU32(test.ctx, 5, PS2_FIO_O_RDONLY);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            const int32_t fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "a profile-provided ROM0 file should open through normal FileIO");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, kBufferAddr);
            setRegU32(test.ctx, 6, 7u);
            fioRead(test.rdram.data(), &test.ctx, &test.runtime);
            t.Equals(getRegS32(&test.ctx, 2), 7, "profile-provided ROM0 bytes should be readable");
            t.IsTrue(std::memcmp(test.rdram.data() + kBufferAddr, "profile", 7u) == 0,
                     "ROM0 profile contents should reach the guest unchanged");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);
        });

        tc.Run("ROM0 uses VFS stat and per-runtime descriptors", [](TestCase &t)
        {
            TestContext owner;
            TestContext other;
            constexpr uint32_t kPathAddr = GUEST_STRING_AREA_START;
            constexpr uint32_t kBufferAddr = GUEST_BUFFER_AREA_START;
            constexpr uint32_t kStatAddr = GUEST_BUFFER_AREA_START + 0x100u;
            writeGuestString(owner.rdram.data(), kPathAddr, "rom0:ROMVER");

            setRegU32(owner.ctx, 4, kPathAddr);
            setRegU32(owner.ctx, 5, PS2_FIO_O_RDONLY);
            fioOpen(owner.rdram.data(), &owner.ctx, &owner.runtime);
            const int32_t fd = getRegS32(&owner.ctx, 2);
            t.IsTrue(fd >= 3, "ROM0 should return a normal VFS descriptor");

            setRegU32(owner.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(owner.ctx, 5, kBufferAddr);
            setRegU32(owner.ctx, 6, 4u);
            fioRead(owner.rdram.data(), &owner.ctx, &other.runtime);
            t.Equals(getRegS32(&owner.ctx, 2), -1,
                     "a descriptor must not leak into a different runtime instance");

            setRegU32(owner.ctx, 4, kPathAddr);
            setRegU32(owner.ctx, 5, kStatAddr);
            fioGetstat(owner.rdram.data(), &owner.ctx, &owner.runtime);
            t.Equals(getRegS32(&owner.ctx, 2), 0, "fioGetstat should see ROM0 virtual files");
            GuestIoStat stat{};
            std::memcpy(&stat, owner.rdram.data() + kStatAddr, sizeof(stat));
            t.Equals(stat.size, 14u, "ROMVER stat should report its exact payload size");
            t.Equals(stat.mode & 0x38u, 0x10u, "ROMVER should be reported as an ioman regular file");

            setRegU32(owner.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(owner.rdram.data(), &owner.ctx, &owner.runtime);
        });

        tc.Run("mc0 directory creation", [](TestCase &t)
        {
            TestContext test;

            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);

            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, &test.runtime);
            
            const int32_t result = getRegS32(&test.ctx, 2);
            t.IsTrue(result >= 0, "fioMkdir should succeed for mc0: directory");

            const std::filesystem::path expected = test.paths.mcRoot / "SAVEDATA";
            t.IsTrue(std::filesystem::exists(expected), 
                "Directory should exist under mcRoot");
            t.IsTrue(std::filesystem::is_directory(expected), 
                "Created path should be a directory");
        });

        tc.Run("mc0 file write operations", [](TestCase &t)
        {
            TestContext test;

            // Setup: create directory first
            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, &test.runtime);

            // Test: open file for writing
            const std::string filePath = "mc0:/SAVEDATA/test.txt";
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            
            const int32_t fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "fioOpen should return valid file descriptor");

            // Write payload
            const std::string payload = "hello mc0";
            const uint32_t bufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + bufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, bufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, &test.runtime);
            
            const int32_t bytesWritten = getRegS32(&test.ctx, 2);
            t.Equals(bytesWritten, static_cast<int32_t>(payload.size()), 
                "fioWrite should write all bytes");

            // Close file
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);
            
            const int32_t closeResult = getRegS32(&test.ctx, 2);
            t.IsTrue(closeResult >= 0, "fioClose should succeed");

            // Verify on host filesystem
            const std::filesystem::path expectedPath = 
                test.paths.mcRoot / "SAVEDATA" / "test.txt";
            t.IsTrue(std::filesystem::exists(expectedPath), 
                "File should exist under mcRoot");

            std::ifstream in(expectedPath, std::ios::binary);
            std::string readback(
                (std::istreambuf_iterator<char>(in)), 
                std::istreambuf_iterator<char>());
            t.Equals(readback, payload, "File content should match written payload");
        });

        tc.Run("mc0 file read operations", [](TestCase &t)
        {
            TestContext test;

            // Setup: create directory and write file
            const std::string dirPath = "mc0:/SAVEDATA";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, &test.runtime);

            const std::string filePath = "mc0:/SAVEDATA/test.txt";
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            // Write data
            const std::string payload = "hello mc0 read test";
            const uint32_t writeBufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + writeBufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            int32_t fd = getRegS32(&test.ctx, 2);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, writeBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, &test.runtime);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);

            // Test: read back via fioRead
            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_O_RDONLY);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            fd = getRegS32(&test.ctx, 2);
            t.IsTrue(fd >= 0, "fioOpen for reading should succeed");

            // Read into different buffer area
            const uint32_t readBufAddr = GUEST_BUFFER_AREA_START + 0x1000;
            std::memset(test.rdram.data() + readBufAddr, 0, payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, readBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioRead(test.rdram.data(), &test.ctx, &test.runtime);

            const int32_t bytesRead = getRegS32(&test.ctx, 2);
            t.Equals(bytesRead, static_cast<int32_t>(payload.size()), 
                "fioRead should read all bytes");

            std::string readback(
                reinterpret_cast<const char*>(test.rdram.data() + readBufAddr),
                payload.size()
            );
            t.Equals(readback, payload, "fioRead content should match original");

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);
        });

        tc.Run("mc0 paths isolated from cdRoot", [](TestCase &t)
        {
            TestContext test;

            const std::string dirPath = "mc0:/ISOLATED";
            const std::string filePath = "mc0:/ISOLATED/test.txt";
            const uint32_t dirAddr = GUEST_STRING_AREA_START;
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x100;
            
            writeGuestString(test.rdram.data(), dirAddr, dirPath);
            writeGuestString(test.rdram.data(), fileAddr, filePath);

            // Create directory and file on mc0:
            setRegU32(test.ctx, 4, dirAddr);
            fioMkdir(test.rdram.data(), &test.ctx, &test.runtime);

            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, PS2_FIO_WRITE_CREATE_TRUNC);
            fioOpen(test.rdram.data(), &test.ctx, &test.runtime);
            const int32_t fd = getRegS32(&test.ctx, 2);

            const std::string payload = "isolation test";
            const uint32_t bufAddr = GUEST_BUFFER_AREA_START;
            std::memcpy(test.rdram.data() + bufAddr, payload.data(), payload.size());

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, bufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            fioWrite(test.rdram.data(), &test.ctx, &test.runtime);

            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            fioClose(test.rdram.data(), &test.ctx, &test.runtime);

            // Verify isolation
            const std::filesystem::path expectedMc = 
                test.paths.mcRoot / "ISOLATED" / "test.txt";
            const std::filesystem::path unexpectedCd = 
                test.paths.cdRoot / "ISOLATED" / "test.txt";

            t.IsTrue(std::filesystem::exists(expectedMc), 
                "mc0: file should exist under mcRoot");
            t.IsFalse(std::filesystem::exists(unexpectedCd), 
                "mc0: file should NOT exist under cdRoot");

            // Verify mcRoot directory structure
            t.IsTrue(std::filesystem::exists(test.paths.mcRoot / "ISOLATED"), 
                "mc0: directory should exist under mcRoot");
            t.IsFalse(std::filesystem::exists(test.paths.cdRoot / "ISOLATED"), 
                "mc0: directory should NOT exist under cdRoot");
        });

        tc.Run("sceMc open write read and close roundtrip through sync", [](TestCase &t)
        {
            TestContext test;

            const uint32_t dirAddr = GUEST_STRING_AREA_START + 0x400;
            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x500;
            const uint32_t writeBufAddr = GUEST_BUFFER_AREA_START + 0x300;
            const uint32_t readBufAddr = GUEST_BUFFER_AREA_START + 0x500;
            const std::string payload = "libmc roundtrip";

            writeGuestString(test.rdram.data(), dirAddr, "/SAVEDATA");
            writeGuestString(test.rdram.data(), fileAddr, "/SAVEDATA/test.bin");
            std::memcpy(test.rdram.data() + writeBufAddr, payload.data(), payload.size());

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, dirAddr);
            ps2_stubs::sceMcMkdir(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcMkdir should dispatch successfully");

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcMkdir should finish successfully");
            t.Equals(cmd, 0x0B, "sceMcSync should report MKDIR as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, fileAddr);
            setRegU32(test.ctx, 7, PS2_FIO_O_RDWR | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC);
            ps2_stubs::sceMcOpen(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcOpen should dispatch successfully");

            const int32_t fd = syncMc(test.rdram, &cmd);
            t.IsTrue(fd > 0, "sceMcOpen should produce a positive descriptor in sceMcSync");
            t.Equals(cmd, 0x02, "sceMcSync should report OPEN as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, writeBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcWrite(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "sceMcWrite should report the full byte count via sceMcSync");
            t.Equals(cmd, 0x06, "sceMcSync should report WRITE as the last command");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, PS2_FIO_SEEK_SET);
            ps2_stubs::sceMcSeek(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcSeek should rewind to offset zero");
            t.Equals(cmd, 0x04, "sceMcSync should report SEEK as the last command");

            std::memset(test.rdram.data() + readBufAddr, 0, payload.size());
            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, readBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcRead(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "sceMcRead should report the full byte count via sceMcSync");
            t.Equals(cmd, 0x05, "sceMcSync should report READ as the last command");

            std::string readback(reinterpret_cast<const char *>(test.rdram.data() + readBufAddr), payload.size());
            t.Equals(readback, payload, "sceMcRead should fill the guest buffer with the written payload");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            ps2_stubs::sceMcClose(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcClose should finish successfully");
            t.Equals(cmd, 0x03, "sceMcSync should report CLOSE as the last command");

            const std::filesystem::path hostPath = test.paths.mcRoot / "SAVEDATA" / "test.bin";
            t.IsTrue(std::filesystem::exists(hostPath), "sceMcOpen/sceMcWrite should create the host file under mcRoot");
        });

        tc.Run("sceMc refuses a command until sceMcSync reports the previous one", [](TestCase &t)
        {
            TestContext test;
            clearContext(test.ctx);
            ps2_stubs::sceMcInit(test.rdram.data(), &test.ctx, nullptr);

            const uint32_t fileAddr = GUEST_STRING_AREA_START + 0x600;
            const uint32_t writeBufAddr = GUEST_BUFFER_AREA_START + 0x700;
            const std::string payload = "profile data";
            writeGuestString(test.rdram.data(), fileAddr, "/busy.bin");
            std::memcpy(test.rdram.data() + writeBufAddr, payload.data(), payload.size());

            clearContext(test.ctx);
            setRegU32(test.ctx, 6, fileAddr);
            setRegU32(test.ctx, 7, PS2_FIO_O_RDWR | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC);
            ps2_stubs::sceMcOpen(test.rdram.data(), &test.ctx, nullptr);
            int32_t cmd = 0;
            const int32_t fd = syncMc(test.rdram, &cmd);
            t.IsTrue(fd >= 0, "sceMcOpen should succeed");

            // Burnout's save: sceMcWrite, then sceMcFlush at once, then sceMcSync.
            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            setRegU32(test.ctx, 5, writeBufAddr);
            setRegU32(test.ctx, 6, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceMcWrite(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcWrite should start");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            ps2_stubs::sceMcFlush(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), -200, "sceMcFlush must be refused while the write is unreported");

            t.Equals(syncMc(test.rdram, &cmd), static_cast<int32_t>(payload.size()),
                     "sceMcSync should report the bytes written");
            t.Equals(cmd, 0x06, "sceMcSync should report WRITE, not the refused FLUSH");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, static_cast<uint32_t>(fd));
            ps2_stubs::sceMcClose(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "a command after sceMcSync should start");
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcClose should succeed");
        });

        tc.Run("sceMcGetDir includes dot entries and file metadata", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.mcRoot / "SAVEDATA");
            const std::string hostPayload = "abc123";
            {
                std::ofstream out(test.paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary);
                out.write(hostPayload.data(), static_cast<std::streamsize>(hostPayload.size()));
            }

            const uint32_t patternAddr = GUEST_STRING_AREA_START + 0x700;
            writeGuestString(test.rdram.data(), patternAddr, "/SAVEDATA/*");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, patternAddr);
            setRegU32(test.ctx, 7, 0u);
            // EE n32 ABI: arguments 5 and 6 travel in $t0/$t1
            setRegU32(test.ctx, 8, 8u);
            setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);

            ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            const int32_t entryCount = syncMc(test.rdram, &cmd);
            t.Equals(cmd, 0x0D, "sceMcSync should report GETDIR as the last command");
            t.Equals(entryCount, 3, "sceMcGetDir should return '.', '..', and the matching file");

            const auto *entries = reinterpret_cast<const SceMcTblGetDir *>(test.rdram.data() + GUEST_MC_TABLE_ADDR);
            t.Equals(std::string(entries[0].entryName), std::string("."), "sceMcGetDir should return '.' first");
            t.Equals(std::string(entries[1].entryName), std::string(".."), "sceMcGetDir should return '..' second");
            t.Equals(std::string(entries[2].entryName), std::string("game.dat"), "sceMcGetDir should include the matching file entry");
            t.Equals(entries[2].fileSizeByte, static_cast<uint32_t>(hostPayload.size()),
                     "sceMcGetDir should report the host file size");
            t.IsTrue((entries[2].attrFile & 0x0080u) != 0u,
                     "sceMcGetDir file entries should carry the closed-file attribute");
        });

        tc.Run("sceMcGetDir with a negative count returns the entry count without a table", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.mcRoot / "SAVEDATA");
            std::ofstream(test.paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary) << "x";

            const uint32_t patternAddr = GUEST_STRING_AREA_START + 0x700;
            writeGuestString(test.rdram.data(), patternAddr, "/SAVEDATA/*");
            uint8_t *table = test.rdram.data() + GUEST_MC_TABLE_ADDR;
            std::memset(table, 0xAB, 64);

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, patternAddr);
            setRegU32(test.ctx, 7, 0u);
            setRegU32(test.ctx, 8, 0xFFFFFFFFu);
            setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);

            ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 3, "a negative count should report '.', '..' and the file");
            t.Equals(static_cast<int>(table[0]), 0xAB, "a negative count should not write the table");
        });

        tc.Run("sceMcGetDir with a non-zero mode continues the previous search", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.mcRoot / "SAVEDATA");
            std::ofstream(test.paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary) << "x";

            const uint32_t patternAddr = GUEST_STRING_AREA_START + 0x700;
            writeGuestString(test.rdram.data(), patternAddr, "/SAVEDATA/*");
            const auto *entries = reinterpret_cast<const SceMcTblGetDir *>(test.rdram.data() + GUEST_MC_TABLE_ADDR);

            const char *expected[] = {".", "..", "game.dat"};
            for (uint32_t call = 0u; call < 4u; ++call)
            {
                clearContext(test.ctx);
                setRegU32(test.ctx, 4, 0u);
                setRegU32(test.ctx, 5, 0u);
                setRegU32(test.ctx, 6, patternAddr);
                setRegU32(test.ctx, 7, call == 0u ? 0u : 1u);
                setRegU32(test.ctx, 8, 1u);
                setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);
                ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

                int32_t cmd = 0;
                const int32_t count = syncMc(test.rdram, &cmd);
                if (call < 3u)
                {
                    t.Equals(count, 1, "each call should return the next entry");
                    t.Equals(std::string(entries[0].entryName), std::string(expected[call]), "entries should come in order");
                }
                else
                {
                    t.Equals(count, 0, "the search should end after the last entry");
                }
            }
        });

        tc.Run("sceMcGetDir on a directory name returns that directory's entry", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.mcRoot / "SAVEDATA");
            std::ofstream(test.paths.mcRoot / "SAVEDATA" / "game.dat", std::ios::binary) << "x";

            const uint32_t pathAddr = GUEST_STRING_AREA_START + 0x700;
            writeGuestString(test.rdram.data(), pathAddr, "SAVEDATA");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, pathAddr);
            setRegU32(test.ctx, 7, 0u);
            setRegU32(test.ctx, 8, 8u);
            setRegU32(test.ctx, 9, GUEST_MC_TABLE_ADDR);

            ps2_stubs::sceMcGetDir(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 1, "a directory name should match only that directory");
            const auto *entries = reinterpret_cast<const SceMcTblGetDir *>(test.rdram.data() + GUEST_MC_TABLE_ADDR);
            t.Equals(std::string(entries[0].entryName), std::string("SAVEDATA"), "the entry should be the directory");
        });

        tc.Run("time stretch slows the tempo and keeps the pitch", [](TestCase &t)
        {
            // Two seconds of a 440 Hz tone, played at 80% of its tempo.
            std::vector<int16_t> in(96000u * 2u);
            for (size_t i = 0; i < 96000u; ++i)
                in[i * 2u] = in[i * 2u + 1u] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i) / 48000.0));
            ps2x::TimeStretch stretch;
            std::vector<int16_t> out;
            const size_t consumed = stretch.process(in.data(), 96000u, 0.8, 1000000u, out);
            const size_t produced = out.size() / 2u;
            t.IsTrue(produced > 96000u, "the output is longer than what it consumed");
            t.IsTrue(std::abs(double(consumed) / double(produced) - 0.8) < 0.02, "it consumes 0.8 frames a frame");
            size_t crossings = 0u;
            for (size_t i = 1; i < produced; ++i)
                crossings += (out[(i - 1u) * 2u] < 0) != (out[i * 2u] < 0) ? 1u : 0u;
            const double frequency = double(crossings) / 2.0 / (double(produced) / 48000.0);
            t.IsTrue(std::abs(frequency - 440.0) < 8.0, "the tone stays at 440 Hz");
        });

        tc.Run("settings.ini round-trips and reports bad lines", [](TestCase &t)
        {
            const std::filesystem::path path = std::filesystem::temp_directory_path() / "bdr_settings_test.ini";
            ps2x::HostSettings &settings = ps2x::hostSettings();
            const ps2x::HostSettings saved = settings;
            settings.superSampling = 8;
            settings.highResolution = false;
            settings.fxaa = true;
            settings.hudSmoothing = false;
            settings.aspect = "16:9";
            settings.volume = 40;
            settings.keys[4] = {ps2x::kMouseInput, 'J', ps2x::kMouseWheelDown};
            settings.keys[12] = {0, 0, 0};
            t.IsTrue(ps2x::writeHostSettings(path), "defaults are written");
            settings = ps2x::HostSettings{};
            std::string errors;
            t.IsTrue(ps2x::loadHostSettings(path, errors), "the written file loads");
            t.Equals(errors, std::string(), "the written file has no bad line");
            t.Equals(settings.superSampling, 8, "super_sampling round-trips");
            t.IsTrue(!settings.highResolution, "high_resolution round-trips");
            t.IsTrue(settings.fxaa, "fxaa round-trips");
            t.IsTrue(!settings.hudSmoothing, "hud_smoothing round-trips");
            t.Equals(settings.aspect, std::string("16:9"), "aspect round-trips");
            t.Equals(settings.volume, 40, "volume round-trips");
            t.Equals(settings.keys[4][0], ps2x::kMouseInput, "a mouse button binding round-trips");
            t.Equals(settings.keys[4][1], int('J'), "a key binding round-trips");
            t.Equals(settings.keys[4][2], ps2x::kMouseWheelDown, "a wheel binding round-trips");
            t.Equals(settings.keys[12][0], 0, "an unbound button round-trips");
            t.Equals(settings.keys[0][0], 265, "the default bindings are written");
            {
                std::ofstream file(path);
                file << "window_width = 2560  # QHD\nsuper_sampling = 3\nunknown = 1\nfullscreen = yes\n"
                        "key_l1 = F2 mouse_right\nkey_r1 = NOPE\n";
            }
            errors.clear();
            t.IsTrue(ps2x::loadHostSettings(path, errors), "the edited file loads");
            t.Equals(settings.windowWidth, 2560, "comments after a value are ignored");
            t.IsTrue(settings.fullscreen, "yes is true");
            t.Equals(settings.keys[8][0], 291, "F2 is read");
            t.Equals(settings.keys[8][1], ps2x::kMouseInput + 1, "input names ignore case");
            t.IsTrue(errors.find(":6:") != std::string::npos, "an unknown input name is reported");
            t.Equals(settings.superSampling, 8, "an invalid sample count is ignored");
            t.IsTrue(errors.find(":2:") != std::string::npos && errors.find(":3:") != std::string::npos,
                     "bad and unknown lines are reported");
            std::filesystem::remove(path);
            settings = saved;
        });

        tc.Run("sceMcGetInfo reports formatted and unformatted states", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t typeAddr = GUEST_BUFFER_AREA_START + 0x900;
            constexpr uint32_t freeAddr = GUEST_BUFFER_AREA_START + 0x904;
            constexpr uint32_t formatAddr = GUEST_BUFFER_AREA_START + 0x908;

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, typeAddr);
            setRegU32(test.ctx, 7, freeAddr);
            // EE n32 ABI: the fifth argument travels in $t0
            setRegU32(test.ctx, 8, formatAddr);
            ps2_stubs::sceMcGetInfo(test.rdram.data(), &test.ctx, nullptr);

            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "formatted cards should report success through sceMcSync");
            t.Equals(cmd, 0x01, "sceMcSync should report GETINFO as the last command");
            t.Equals(readGuestS32(test.rdram.data(), typeAddr), 2, "sceMcGetInfo should report a PS2 memory card");
            t.Equals(readGuestS32(test.rdram.data(), formatAddr), 1, "sceMcGetInfo should report a formatted card");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            ps2_stubs::sceMcUnformat(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcUnformat should complete successfully");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, typeAddr);
            setRegU32(test.ctx, 7, freeAddr);
            setRegU32(test.ctx, 8, formatAddr);
            ps2_stubs::sceMcGetInfo(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(syncMc(test.rdram, &cmd), -2, "unformatted cards should report sceMcResNoFormat through sceMcSync");
            t.Equals(readGuestS32(test.rdram.data(), formatAddr), 0, "sceMcGetInfo should report an unformatted card after sceMcUnformat");
        });

        tc.Run("sceMcEnd resets libmc state so sync reports no active command", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t dirAddr = GUEST_STRING_AREA_START + 0xB00;

            clearContext(test.ctx);
            ps2_stubs::sceMcInit(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcInit should succeed");

            writeGuestString(test.rdram.data(), dirAddr, "/SAVEDATA");
            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 0u);
            setRegU32(test.ctx, 6, dirAddr);
            ps2_stubs::sceMcMkdir(test.rdram.data(), &test.ctx, nullptr);
            int32_t cmd = 0;
            t.Equals(syncMc(test.rdram, &cmd), 0, "sceMcMkdir should complete before teardown");
            t.Equals(cmd, 0x0B, "sceMcSync should report MKDIR before teardown");

            clearContext(test.ctx);
            ps2_stubs::sceMcEnd(test.rdram.data(), &test.ctx, nullptr);
            t.Equals(getRegS32(&test.ctx, 2), 0, "sceMcEnd should succeed");

            // libmc semantics: with no async command pending, sceMcSync returns -1
            // and leaves the cmd/result out-parameters untouched.
            R5900Context syncCtx{};
            setRegU32(syncCtx, 4, 0u);
            setRegU32(syncCtx, 5, GUEST_MC_SYNC_CMD_ADDR);
            setRegU32(syncCtx, 6, GUEST_MC_SYNC_RESULT_ADDR);
            ps2_stubs::sceMcSync(test.rdram.data(), &syncCtx, nullptr);
            t.Equals(getRegS32(&syncCtx, 2), -1,
                     "sceMcSync after sceMcEnd should report that no command is active");
        });

        tc.Run("sceIoctl cmd1 updates wait flag state", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t statusAddr = GUEST_BUFFER_AREA_START + 0x1800;
            const uint32_t busy = 1u;
            std::memcpy(test.rdram.data() + statusAddr, &busy, sizeof(busy));

            setRegU32(test.ctx, 4, 3u);          // fd
            setRegU32(test.ctx, 5, 1u);          // cmd
            setRegU32(test.ctx, 6, statusAddr);  // arg

            ps2_stubs::sceIoctl(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 0, "sceIoctl cmd1 should return success");

            uint32_t state = 0xFFFFFFFFu;
            std::memcpy(&state, test.rdram.data() + statusAddr, sizeof(state));
            t.Equals(state, 0u, "sceIoctl cmd1 should clear wait state from busy to ready");
        });

        tc.Run("sceCdSearchFile resolves movie filenames with zero-padded host leaf", [](TestCase &t)
        {
            TestContext test;

            std::filesystem::create_directories(test.paths.cdRoot / "movie");
            {
                std::ofstream out(test.paths.cdRoot / "movie" / "mv_016.pss", std::ios::binary);
                const std::string payload = "pss";
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }

            constexpr uint32_t fileAddr = GUEST_BUFFER_AREA_START + 0x1A00;
            constexpr uint32_t pathAddr = GUEST_STRING_AREA_START + 0xA00;
            writeGuestString(test.rdram.data(), pathAddr, "\\MOVIE\\MV_16.PSS;1");

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, fileAddr);
            setRegU32(test.ctx, 5, pathAddr);
            ps2_stubs::sceCdSearchFile(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 1, "sceCdSearchFile should resolve the extracted movie file");
            t.Equals(readGuestU32(test.rdram.data(), fileAddr + 4), 3u,
                     "sceCdSearchFile should report the host file size");
            t.IsTrue(readGuestU32(test.rdram.data(), fileAddr + 0) >= 0x00100000u,
                     "sceCdSearchFile should assign a pseudo LSN for the resolved host file");
        });

        tc.Run("sceCdRead reads from explicit cdImage path", [](TestCase &t)
        {
            TestContext test;

            constexpr uint32_t kSectorSize = 2048u;
            constexpr uint32_t bufAddr = GUEST_BUFFER_AREA_START + 0x1C80;
            const std::filesystem::path imagePath = test.paths.base / "disc.iso";
            {
                std::vector<uint8_t> sector(kSectorSize, 0);
                const char payload[] = "cd-image";
                std::memcpy(sector.data(), payload, sizeof(payload) - 1);

                std::ofstream out(imagePath, std::ios::binary);
                out.write(reinterpret_cast<const char *>(sector.data()),
                          static_cast<std::streamsize>(sector.size()));
            }

            PS2Runtime::IoPaths ioPaths;
            ioPaths.elfDirectory = test.paths.cdRoot;
            ioPaths.hostRoot = test.paths.cdRoot;
            ioPaths.cdRoot = test.paths.cdRoot;
            ioPaths.mcRoot = test.paths.mcRoot;
            ioPaths.cdImage = imagePath;
            PS2Runtime::setIoPaths(ioPaths);

            clearContext(test.ctx);
            setRegU32(test.ctx, 4, 0u);
            setRegU32(test.ctx, 5, 1u);
            setRegU32(test.ctx, 6, bufAddr);
            ps2_stubs::sceCdRead(test.rdram.data(), &test.ctx, nullptr);

            t.Equals(getRegS32(&test.ctx, 2), 1, "sceCdRead should succeed when cdImage is configured");
            t.Equals(std::memcmp(test.rdram.data() + bufAddr, "cd-image", 8), 0,
                     "sceCdRead should copy sector data from the configured image");
        });
    });
}
