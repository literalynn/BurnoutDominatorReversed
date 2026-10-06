#include "ps2x/iop/iso9660.h"
#include "ps2x/iop/iop_host.h"
#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;
    constexpr size_t Sector = Iso9660SectorSize;

    void require(bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    void both16(uint8_t *p, uint16_t value)
    {
        p[0] = static_cast<uint8_t>(value);
        p[1] = static_cast<uint8_t>(value >> 8u);
        p[2] = p[1];
        p[3] = p[0];
    }

    void both32(uint8_t *p, uint32_t value)
    {
        for (size_t i = 0u; i < 4u; ++i)
        {
            p[i] = static_cast<uint8_t>(value >> (i * 8u));
            p[7u - i] = p[i];
        }
    }

    std::vector<uint8_t> record(std::string name, uint32_t lsn, uint32_t size, uint8_t flags = 0u)
    {
        const size_t length = 33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u);
        std::vector<uint8_t> bytes(length, 0u);
        bytes[0] = static_cast<uint8_t>(length);
        both32(bytes.data() + 2u, lsn);
        both32(bytes.data() + 10u, size);
        const std::array<uint8_t, 7u> date{107u, 3u, 6u, 12u, 0u, 0u, 0u};
        std::copy(date.begin(), date.end(), bytes.begin() + 18u);
        bytes[25u] = flags;
        both16(bytes.data() + 28u, 1u);
        bytes[32u] = static_cast<uint8_t>(name.size());
        std::memcpy(bytes.data() + 33u, name.data(), name.size());
        return bytes;
    }

    struct Image
    {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(64u * Sector, 0u);
        std::map<std::string, size_t> positions;

        Image()
        {
            uint8_t *pvd = bytes.data() + 16u * Sector;
            std::memcpy(pvd, "\x01" "CD001\x01", 7u);
            both32(pvd + 80u, 64u);
            both16(pvd + 120u, 1u);
            both16(pvd + 124u, 1u);
            both16(pvd + 128u, static_cast<uint16_t>(Sector));
            const auto root = record(std::string(1u, '\0'), 20u, static_cast<uint32_t>(Sector), 2u);
            std::copy(root.begin(), root.end(), pvd + 156u);
            std::memcpy(bytes.data() + 17u * Sector, "\xff" "CD001\x01", 7u);
            const std::vector<std::pair<std::string, std::vector<uint8_t>>> records{
                {"self", root},
                {"parent", record(std::string(1u, '\x01'), 20u, static_cast<uint32_t>(Sector), 2u)},
                {"SYSTEM.CNF", record("SYSTEM.CNF;1", 21u, 41u)},
                {"SLES_546.27", record("SLES_546.27;1", 22u, 1024u)},
                {"DIR", record("DIR", 23u, static_cast<uint32_t>(Sector), 2u)}};
            size_t cursor = 20u * Sector;
            for (const auto &[key, entry] : records)
            {
                positions[key] = cursor;
                std::copy(entry.begin(), entry.end(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
                cursor += entry.size();
            }
            const auto nested = record("file.bin;1", 24u, 7u);
            std::copy(nested.begin(), nested.end(), bytes.begin() + 23u * Sector);
            const std::string cnf = "BOOT2=cdrom0:\\SLES_546.27;1\r\nVER=1.00\r\n";
            std::copy(cnf.begin(), cnf.end(), bytes.begin() + 21u * Sector);
            const std::array<uint8_t, 4u> magic{0x7fu, 'E', 'L', 'F'};
            std::copy(magic.begin(), magic.end(), bytes.begin() + 22u * Sector);
            std::memcpy(bytes.data() + 24u * Sector, "payload", 7u);
        }

        Iso9660ReadAt reader()
        {
            return [this](uint64_t offset, void *destination, size_t count)
            {
                if (offset > bytes.size() || count > bytes.size() - offset)
                    return false;
                std::memcpy(destination, bytes.data() + static_cast<size_t>(offset), count);
                return true;
            };
        }

        bool find(std::string_view path, Iso9660File &file, std::string *error = nullptr)
        {
            return findIso9660File(reader(), bytes.size(), path, file, error);
        }
    };

    void testNormalizationAndData()
    {
        Image image;
        Iso9660File file;
        require(image.find("CdRoM0:\\dir\\FiLe.BiN;1", file), "mixed-case CD path did not resolve");
        require(file.lsn == 24u && file.size == 7u && file.name == "FILE.BIN" && file.path == "DIR/FILE.BIN",
                "file record does not preserve actual LBA/size/canonical path");
        require(file.date[0u] == 107u && file.date[1u] == 3u, "ISO recording date was lost");
        require(image.find("/DIR//FILE.BIN", file), "slash normalization failed");
        require(image.find("cdrom:/SLES_546.27;1", file) && file.lsn == 22u, "cdrom: device alias failed");
        require(!image.find("not-present", file) && file.lsn == 0u, "failed search left stale output");
        for (const auto path : {"../SYSTEM.CNF", "DIR/../SYSTEM.CNF", "..;1/SYSTEM.CNF", "C:/SYSTEM.CNF", "cdrom1:/SYSTEM.CNF", "DIR/./FILE.BIN"})
            require(!image.find(path, file), "unsafe/device path accepted");
    }

    void testMalformedDescriptorsAndBounds()
    {
        Iso9660File file;
        std::string error;
        Image image;
        image.bytes[16u * Sector + 80u] = 65u;
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("dual-endian") != std::string::npos,
                "PVD endian mismatch accepted");
        image = Image{};
        both32(image.bytes.data() + 16u * Sector + 80u, 65u);
        require(!image.find("SYSTEM.CNF", file), "volume beyond file length accepted");
        image = Image{};
        image.bytes[17u * Sector] = 2u;
        require(!image.find("SYSTEM.CNF", file), "missing descriptor terminator accepted");
        image = Image{};
        both16(image.bytes.data() + 16u * Sector + 128u, 512u);
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("block size") != std::string::npos,
                "unsupported sector size accepted");
        image = Image{};
        both16(image.bytes.data() + 16u * Sector + 120u, 2u);
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("multi-volume") != std::string::npos,
                "unsupported volume set accepted");
        image = Image{};
        both32(image.bytes.data() + image.positions.at("SYSTEM.CNF") + 2u, 64u);
        require(!image.find("SYSTEM.CNF", file), "file extent beyond volume accepted");
        image = Image{};
        image.bytes[image.positions.at("SYSTEM.CNF") + 6u] = 1u;
        require(!image.find("SYSTEM.CNF", file), "directory extent endian mismatch accepted");
    }

    void testUnsupportedLayoutsAndMalformedRecords()
    {
        Iso9660File file;
        std::string error;
        Image image;
        image.bytes[image.positions.at("SYSTEM.CNF") + 25u] = 0x80u;
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("multi-extent") != std::string::npos,
                "multi-extent record did not fail explicitly");
        image = Image{};
        image.bytes[image.positions.at("SYSTEM.CNF") + 26u] = 1u;
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("interleaved") != std::string::npos,
                "interleaved record accepted");
        image = Image{};
        image.bytes[20u * Sector] = 33u;
        require(!image.find("SYSTEM.CNF", file), "undersized directory record accepted");
        image = Image{};
        image.bytes[image.positions.at("SYSTEM.CNF") + 32u] = 200u;
        require(!image.find("SYSTEM.CNF", file), "identifier crossing record accepted");
        image = Image{};
        both32(image.bytes.data() + image.positions.at("DIR") + 2u, 20u);
        require(!image.find("DIR/SYSTEM.CNF", file, &error) && error.find("cycle") != std::string::npos,
                "directory cycle accepted");
        image = Image{};
        auto duplicate = record("system.cnf;2", 25u, 2u);
        const size_t end = image.positions.at("DIR") + image.bytes[image.positions.at("DIR")];
        std::copy(duplicate.begin(), duplicate.end(), image.bytes.begin() + static_cast<std::ptrdiff_t>(end));
        require(!image.find("SYSTEM.CNF", file, &error) && error.find("duplicate") != std::string::npos,
                "ambiguous normalized filenames accepted");
    }

    void testSectorPaddingAndExtendedAttributes()
    {
        Image image;
        both32(image.bytes.data() + 16u * Sector + 156u + 10u, 2u * static_cast<uint32_t>(Sector));
        std::fill(image.bytes.begin() + 21u * Sector, image.bytes.begin() + 22u * Sector, 0u);
        const auto nextSector = record("PADDED.BIN;1", 25u, 5u);
        std::copy(nextSector.begin(), nextSector.end(), image.bytes.begin() + 21u * Sector);
        Iso9660File file;
        require(image.find("PADDED.BIN", file) && file.lsn == 25u, "zero directory padding stopped the entire directory");
        image = Image{};
        image.bytes[image.positions.at("SYSTEM.CNF") + 1u] = 1u;
        require(image.find("SYSTEM.CNF", file) && file.lsn == 22u, "extended-attribute blocks did not shift data LBA");
    }

    void testRawReadBoundsAndShortReads()
    {
        Image image;
        std::array<uint8_t, Sector> sector{};
        require(readIso9660Sectors(image.reader(), image.bytes.size(), 22u, 1u, sector.data(), sector.size()),
                "bounded original sector did not read");
        require(sector[0] == 0x7fu && sector[1] == 'E', "raw sector did not use original data");
        require(!readIso9660Sectors(image.reader(), image.bytes.size(), 64u, 1u, sector.data(), 4u),
                "clipped buffer concealed sector range beyond image");
        require(!readIso9660Sectors(image.reader(), image.bytes.size(), 22u, 0u, sector.data(), 4u),
                "byteCount beyond requested sectors accepted");
        require(!readIso9660Sectors([](uint64_t, void *, size_t) { return false; }, image.bytes.size(), 22u, 1u, sector.data(), sector.size()),
                "short host read accepted");
        require(!readIso9660Sectors(image.reader(), image.bytes.size(), std::numeric_limits<uint32_t>::max(), 2u, sector.data(), 4u),
                "large sector arithmetic wrapped");
    }

    void testSparseSourceAbove4GiB()
    {
        Image image;
        constexpr uint32_t highLsn = 0x00200010u;
        const uint64_t imageSize = (static_cast<uint64_t>(highLsn) + 2u) * Sector;
        both32(image.bytes.data() + 16u * Sector + 80u, highLsn + 2u);
        both32(image.bytes.data() + 16u * Sector + 156u + 2u, highLsn);
        std::array<uint8_t, Sector> highDirectory{};
        const auto entry = record("HIGH.BIN;1", highLsn + 1u, 8u);
        std::copy(entry.begin(), entry.end(), highDirectory.begin());
        uint64_t lastOffset = 0u;
        const Iso9660ReadAt reader = [&](uint64_t offset, void *destination, size_t count)
        {
            lastOffset = offset;
            if (offset == static_cast<uint64_t>(highLsn) * Sector && count <= Sector)
                std::memcpy(destination, highDirectory.data(), count);
            else if (offset == static_cast<uint64_t>(highLsn + 1u) * Sector && count <= 8u)
                std::memcpy(destination, "highdata", count);
            else if (offset <= image.bytes.size() && count <= image.bytes.size() - offset)
                std::memcpy(destination, image.bytes.data() + static_cast<size_t>(offset), count);
            else
                return false;
            return true;
        };
        Iso9660File file;
        require(findIso9660File(reader, imageSize, "HIGH.BIN", file) && file.lsn == highLsn + 1u,
                "lookup truncated >4GiB directory offsets");
        require(lastOffset > std::numeric_limits<uint32_t>::max(), "sparse high directory was not read through uint64 offset");
        std::array<char, 8u> payload{};
        require(readIso9660Sectors(reader, imageSize, file.lsn, 1u, payload.data(), payload.size()) &&
                std::string(payload.data(), payload.size()) == "highdata", "read truncated >4GiB data offsets");
    }

    class ImageHost final : public IopHost
    {
    public:
        Image image;
        std::string imagePath = "configured.iso";
        std::filesystem::path root;
        bool failReads = false;
        bool failOpen = false;
        bool failSize = false;
        uint64_t lastReadOffset = 0u;
        size_t lastReadSize = 0u;
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind kind) const override
        {
            if (kind == HostPathKind::CdImage)
                return imagePath;
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return failOpen ? 0u : 1u; }
        bool hostFileSize(uint64_t, uint64_t &size) const override { size = image.bytes.size(); return !failSize; }
        bool readHostFile(uint64_t, uint64_t offset, void *destination, size_t size, size_t &read) override
        {
            lastReadOffset = offset;
            lastReadSize = size;
            read = 0u;
            if (failReads || !image.reader()(offset, destination, size))
                return false;
            read = size;
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    struct LocalFixture
    {
        std::filesystem::path parent = std::filesystem::current_path();
        std::filesystem::path root = parent / ("iso-native-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        bool created = false;
        LocalFixture()
        {
            require(root.parent_path() == parent && std::filesystem::create_directory(root), "cannot create scoped native fixture");
            created = true;
            std::ofstream(root / "SYSTEM.CNF") << "loose-fallback";
        }
        ~LocalFixture()
        {
            if (created && root.parent_path() == parent)
            {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }
        }
    };

    void testIopOriginalImageAuthorityAndNoImageFallback()
    {
        LocalFixture fixture;
        ImageHost host;
        host.root = fixture.root;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();
        constexpr uint32_t resultAddress = 0x1000u;
        constexpr uint32_t nameAddress = 0x1100u;
        constexpr uint32_t readAddress = 0x2000u;
        require(memory.zeroRam(readAddress, Sector), "cannot initialize owned IOP read buffer");
        const char path[] = "cdrom0:\\SYSTEM.CNF;1";
        require(memory.writeRam(nameAddress, path, sizeof(path)), "cannot write test guest path");
        const auto search = [&]
        {
            IopCpuState cpu{};
            cpu.gpr[4u] = resultAddress;
            cpu.gpr[5u] = nameAddress;
            require(cdvd.dispatchImport(10u, cpu), "sceCdSearchFile was not handled");
            return cpu.gpr[2u];
        };
        const auto read = [&](uint32_t lsn)
        {
            IopCpuState cpu{};
            cpu.gpr[4u] = lsn;
            cpu.gpr[5u] = 1u;
            cpu.gpr[6u] = readAddress;
            require(cdvd.dispatchImport(6u, cpu), "sceCdRead was not handled");
            return cpu.gpr[2u];
        };
        require(search() == 1u && memory.read32(resultAddress) == 21u && memory.read32(resultAddress + 4u) == 41u,
                "IOP search returned synthetic LBA/size for original image");
        const uint32_t rawReadResult = read(22u);
        if (rawReadResult != 1u || memory.read8(readAddress) != 0x7fu)
            std::cerr << "IOP raw read result=" << rawReadResult << " first=" << unsigned(memory.read8(readAddress))
                      << " host_offset=" << host.lastReadOffset << " count=" << host.lastReadSize << '\n';
        require(rawReadResult == 1u && memory.read8(readAddress) == 0x7fu, "IOP raw read did not match original image");
        host.failReads = true;
        require(search() == 0u, "IOP failed-image lookup fell back to extracted tree");
        require(read(16u) == 0u, "IOP failed-image read fell back to synthetic PVD");
        host.failReads = false;
        host.failSize = true;
        require(search() == 0u && read(16u) == 0u, "IOP failed image size fell back to synthetic sectors");
        host.failSize = false;
        host.failOpen = true;
        cdvd.reset();
        require(search() == 0u && read(16u) == 0u, "IOP failed image open fell back to synthetic sectors");
        host.failOpen = false;
        host.imagePath.clear();
        cdvd.reset();
        require(search() == 1u && memory.read32(resultAddress + 4u) == 14u,
                "IOP no-image extracted-tree lookup compatibility was lost");
        require(read(16u) == 1u && memory.read8(readAddress) == 1u && memory.read8(readAddress + 1u) == 'C',
                "IOP no-image virtual PVD compatibility was lost");
    }

    void validateRealImage(const std::filesystem::path &image)
    {
        for (const auto name : {"cdrom0:\\SYSTEM.CNF;1", "cdrom0:\\SLES_546.27;1"})
        {
            Iso9660File file;
            std::string error;
            require(findIso9660File(image, name, file, &error), error.c_str());
            std::cout << file.path << " LBA=" << file.lsn << " SIZE=" << file.size
                      << " OFFSET=" << static_cast<uint64_t>(file.lsn) * Sector << '\n';
            std::array<uint8_t, 4u> first{};
            require(readIso9660Sectors(image, file.lsn, 1u, first.data(), first.size()), "cannot read real image file start");
            if (file.name == "SLES_546.27")
                require(first == std::array<uint8_t, 4u>{0x7fu, 'E', 'L', 'F'}, "real BOOT2 sector is not ELF magic");
        }
    }
}

int main(int argc, char **argv)
{
    try
    {
        testNormalizationAndData();
        testMalformedDescriptorsAndBounds();
        testUnsupportedLayoutsAndMalformedRecords();
        testSectorPaddingAndExtendedAttributes();
        testRawReadBoundsAndShortReads();
        testSparseSourceAbove4GiB();
        testIopOriginalImageAuthorityAndNoImageFallback();
        if (argc == 3 && std::string_view(argv[1]) == "--iso")
            validateRealImage(std::filesystem::path(argv[2]));
        std::cout << "ISO9660 native tests passed (7 groups, including IOP integration and >4GiB offsets).\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
