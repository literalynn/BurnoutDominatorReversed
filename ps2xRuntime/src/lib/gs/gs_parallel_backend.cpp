// GS backend on paraLLEl-GS (Arntzen Software, LGPLv3), which emulates the GS
// with Vulkan compute. Adapted from upstream PS2Recomp pull request #285; this
// version needs no window: it creates its own Vulkan device and reads the
// scanout image back to host memory, so presentation, frame dumps and headless
// runs go through the usual PresentationFrame pixels.
#include "runtime/gs/gs_parallel_backend.h"
#include "ps2x/state_archive.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/host_settings.h"
#include "runtime/ps2_memory.h"
#include "ps2_log.h"
#include "gs_interface.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace
{
    constexpr size_t VramSize = 4 * 1024 * 1024;

    uint64_t word(const void *data)
    {
        uint64_t value;
        std::memcpy(&value, data, 8);
        return value;
    }

    unsigned bitsPerPixel(unsigned psm)
    {
        switch (psm & 63)
        {
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return 24;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 16;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return 8;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return 4;
        default:
            return 32;
        }
    }

    using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    using Writer = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    struct PixelAccess
    {
        Reader read;
        Writer write;
    };

    PixelAccess pixelAccess(unsigned psm)
    {
        using namespace GSMem;
        switch (psm & 63)
        {
#define PSM(id, name) \
    case GS_PSM_##id: \
        return {Read##name, Write##name}
            PSM(CT32, CT32);
            PSM(CT24, CT24);
            PSM(CT16, CT16);
            PSM(CT16S, CT16S);
            PSM(T8, P8);
            PSM(T8H, P8H);
            PSM(T4, P4);
            PSM(T4HL, P4HL);
            PSM(T4HH, P4HH);
            PSM(Z32, Z32);
            PSM(Z24, Z24);
            PSM(Z16, Z16);
            PSM(Z16S, Z16S);
#undef PSM
        default:
            return {ReadNull, WriteNull};
        }
    }

    // A Vulkan instance and device of our own, without a window or swapchain.
    struct VulkanDevice
    {
        Vulkan::Context context;
        Vulkan::Device device;

        VulkanDevice()
        {
            Util::register_thread_index(0);
            if (!Vulkan::Context::init_loader(nullptr))
                throw std::runtime_error("Vulkan loader unavailable");
            context.set_num_thread_indices(1);
            if (!context.init_instance_and_device(nullptr, 0, nullptr, 0,
                                                  Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                                      Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                                      Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
                throw std::runtime_error("Vulkan instance or device initialization failed");
            device.set_context(context);
            device.init_frame_contexts(4);
        }
    };

    class ParallelBackend final : public GSRasterBackend, private ParallelGS::SignalInterface
    {
        // Every GSInterface and Device call is serialized, presentation included.
        mutable std::recursive_mutex mutex;
        VulkanDevice vulkan;
        mutable ParallelGS::GSInterface gs;
        GSRegisters &priv;
        Vulkan::BufferHandle readback;

        struct Lock
        {
            std::lock_guard<std::recursive_mutex> guard;
            explicit Lock(std::recursive_mutex &m) : guard(m) { Util::register_thread_index(0); }
        };

        // Transfer progress, observed from the GIF stream for GetTransferSnapshot().
        uint64_t bitblt = 0, trxreg = 0;
        GSTransferSnapshot transfer;
        size_t transferBytes = 0, uploadedBytes = 0;
        std::array<uint8_t, 8> uploadTail{};
        size_t uploadTailSize = 0;
        std::array<uint8_t, 16> fifoTail{};
        size_t fifoOffset = 16;

        void observeRegister(uint8_t addr, uint64_t value)
        {
            if (addr == GS_REG_BITBLTBUF)
                bitblt = value;
            if (addr == GS_REG_TRXREG)
                trxreg = value;
            if (addr == GS_REG_TRXDIR)
            {
                transfer = {};
                transfer.direction = value & 3;
                transfer.totalPixels = uint32_t(trxreg & 0xfff) * uint32_t((trxreg >> 32) & 0xfff);
                unsigned psm = unsigned(bitblt >> (transfer.direction == 1 ? 24 : 56)) & 63;
                transferBytes = (size_t(transfer.totalPixels) * bitsPerPixel(psm) + 7) / 8;
                transfer.localToHostPendingBytes = transfer.direction == 1 ? transferBytes : 0;
                transfer.copiedPixels = transfer.direction == 2 ? transfer.totalPixels : 0;
                uploadedBytes = uploadTailSize = 0;
                fifoOffset = fifoTail.size();
                if (transfer.direction == 1 && std::getenv("BDR_PGS_TRACE"))
                    std::printf("[pgs-readback] vsync=%llu bitbltbuf=%016llx trxreg=%016llx bytes=%zu\n",
                                (unsigned long long)priv.vsyncTick.load(), (unsigned long long)bitblt,
                                (unsigned long long)trxreg, transferBytes);
            }
            if (addr == GS_REG_HWREG)
                observeImage(8);
        }

        void observeImage(size_t bytes)
        {
            if (transfer.direction != 0)
                return;
            uploadedBytes = std::min(transferBytes, uploadedBytes + bytes);
            transfer.copiedPixels = std::min<size_t>(transfer.totalPixels, uploadedBytes * 8 / bitsPerPixel(unsigned(bitblt >> 56)));
            auto width = uint32_t(trxreg & 0xfff);
            if (width)
            {
                transfer.x = transfer.copiedPixels % width;
                transfer.y = transfer.copiedPixels / width;
            }
        }

        // Follows transfer boundaries only; paraLLEl-GS alone executes the
        // commands. Starts from its saved state for the path, so packets split
        // across DMA calls are followed correctly.
        void observeGIF(uint32_t path, const uint8_t *data, size_t size)
        {
            auto state = gs.get_gif_path(path);
            for (size_t offset = 0; offset < size;)
            {
                if (state.loop == state.tag.NLOOP)
                {
                    std::memcpy(&state.tag, data + offset, 16);
                    state.reg = state.loop = 0;
                    offset += 16;
                    continue;
                }
                unsigned nreg = state.tag.NREG ? state.tag.NREG : 16;
                if (state.tag.FLG >= 2)
                {
                    size_t count = std::min<size_t>(state.tag.NLOOP - state.loop, (size - offset) / 16);
                    observeImage(count * 16);
                    state.loop += uint32_t(count);
                    offset += count * 16;
                }
                else
                {
                    // Only packed A+D can address the transfer registers (>= 0x50).
                    const uint64_t descriptors = word(reinterpret_cast<const uint8_t *>(&state.tag) + 8);
                    if (state.tag.FLG == 0 && ((descriptors >> (state.reg * 4)) & 15) == 14)
                        observeRegister(uint8_t(word(data + offset + 8) & 127), word(data + offset));
                    unsigned count = state.tag.FLG == 0 ? 1 : 2;
                    while (count-- && state.loop < state.tag.NLOOP)
                        if (++state.reg == nreg)
                        {
                            state.reg = 0;
                            ++state.loop;
                        }
                    offset += 16;
                }
            }
        }

        // BDR_PGS_RECORD=file: every input of the backend, in order, as records
        // {char kind; uint32 id; uint32 size; bytes}: 'I' initial VRAM, 'G' GIF
        // packet (id = path), 'R' register (id = address), 'U' image data, 'P'
        // presentation. ps2x_tests replays it (BDR_PGS_REPLAY) to compare backends.
        // BDR_PGS_RECORD_FROM=N and BDR_PGS_RECORD_VSYNCS=M limit the recording
        // to VSync ticks N to N+M; it then starts with the VRAM of tick N ('I').
        void traceCall(char kind, uint32_t id, const uint8_t *data, uint32_t size)
        {
            static FILE *record = [] {
                const char *path = std::getenv("BDR_PGS_RECORD");
                return path ? std::fopen(path, "wb") : nullptr;
            }();
            if (!record)
                return;
            static const uint64_t from = std::getenv("BDR_PGS_RECORD_FROM") ? std::strtoull(std::getenv("BDR_PGS_RECORD_FROM"), nullptr, 10) : 0;
            static const uint64_t vsyncs = std::getenv("BDR_PGS_RECORD_VSYNCS") ? std::strtoull(std::getenv("BDR_PGS_RECORD_VSYNCS"), nullptr, 10) : ~0ull;
            static bool started = from == 0;
            const uint64_t tick = priv.vsyncTick.load(std::memory_order_relaxed);
            if (tick < from || tick - from > vsyncs)
            {
                if (started)
                    std::fflush(record); // the runner can exit without closing the file
                return;
            }
            if (!started)
            {
                started = true;
                const char initial = 'I';
                const uint32_t zero = 0, bytes = uint32_t(VramSize);
                std::fwrite(&initial, 1, 1, record);
                std::fwrite(&zero, 4, 1, record);
                std::fwrite(&bytes, 4, 1, record);
                std::fwrite(gs.map_vram_read(0, VramSize), 1, VramSize, record);
            }
            std::fwrite(&kind, 1, 1, record);
            std::fwrite(&id, 4, 1, record);
            std::fwrite(&size, 4, 1, record);
            if (size)
                std::fwrite(data, 1, size, record);
            if (kind == 'P')
                std::fflush(record);
        }

        bool on_signal(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (priv.siglblid & 0xffffffff00000000ull) | id;
            priv.csr.fetch_or(1);
            return false;
        }

        bool on_finish(uint64_t) override
        {
            // FINISH: everything before it is submitted; later readbacks wait for the GPU themselves.
            priv.csr.fetch_or(2);
            return true;
        }

        bool on_label(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid >> 32) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (uint64_t(id) << 32) | uint32_t(priv.siglblid);
            return false;
        }

    public:
        explicit ParallelBackend(GSRegisters &registers) : priv(registers)
        {
            Lock lock(mutex);
            ParallelGS::GSOptions options{};
            options.super_sampling = ParallelGS::SuperSampling(ps2x::hostSettings().superSampling);
            if (!gs.init(&vulkan.device, options))
                throw std::runtime_error("paraLLEl-GS initialization failed");
            gs.set_signal_interface(this);
        }

        ~ParallelBackend() override
        {
            Lock lock(mutex);
            gs.flush();
            vulkan.device.wait_idle();
        }

        bool UsesRawCommands() const override
        {
            return true;
        }

        void Initialize(uint8_t *vram, uint32_t size) override
        {
            Lock lock(mutex);
            if (!vram || size < VramSize)
                throw std::invalid_argument("paraLLEl-GS requires 4 MiB of initial VRAM");
            traceCall('I', 0, vram, VramSize);
            std::memcpy(gs.map_vram_write(0, VramSize), vram, VramSize);
            gs.end_vram_write(0, VramSize);
        }

        void Reset() override
        {
            Lock lock(mutex);
            gs.flush();
            vulkan.device.wait_idle();
            gs.reset_context_state();
            bitblt = trxreg = transferBytes = uploadedBytes = uploadTailSize = 0;
            fifoOffset = 16;
            transfer = {};
        }

        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t size) override
        {
            Lock lock(mutex);
            if (path < 1 || path > 3 || size % 16)
                throw std::invalid_argument("paraLLEl-GS: invalid GIF path or size");
            if (!data || !size)
                return;
            traceCall('G', path, data, size);
            observeGIF(path, data, size);
            gs.gif_transfer(path, data, size);
        }

        void WriteRegisterRaw(uint8_t addr, uint64_t value) override
        {
            Lock lock(mutex);
            if (addr >= 128)
                return; // Reserved addresses never index paraLLEl-GS's 128-entry table.
            traceCall('R', addr, reinterpret_cast<const uint8_t *>(&value), 8);
            observeRegister(addr, value);
            // Aliases used by runtime stubs for the privileged display registers.
            switch (addr)
            {
            case 0x59:
                priv.dispfb1 = value;
                return;
            case 0x5a:
                priv.display1 = value;
                return;
            case 0x5b:
                priv.dispfb2 = value;
                return;
            case 0x5c:
                priv.display2 = value;
                return;
            case 0x5f:
                priv.bgcolor = value;
                return;
            }
            gs.write_register(static_cast<ParallelGS::RegisterAddr>(addr), value);
        }

        void Submit(const GSPrimitiveBatch &) override
        {
            throw std::logic_error("paraLLEl-GS received a decoded primitive");
        }

        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override
        {
        }

        void BeginTransfer(const GSTransferCommand &) override
        {
            throw std::logic_error("paraLLEl-GS takes transfers as raw registers");
        }

        void UploadImage(const uint8_t *data, uint32_t size) override
        {
            Lock lock(mutex);
            if (!data || transfer.direction != 0)
                return;
            size = uint32_t(std::min<size_t>(size, transferBytes - uploadedBytes));
            traceCall('U', 0, data, size);
            observeImage(size);
            while (size)
            {
                auto n = std::min<size_t>(size, 8 - uploadTailSize);
                std::memcpy(uploadTail.data() + uploadTailSize, data, n);
                uploadTailSize += n;
                data += n;
                size -= uint32_t(n);
                if (uploadTailSize == 8 || (uploadedBytes == transferBytes && size == 0))
                {
                    std::fill(uploadTail.begin() + uploadTailSize, uploadTail.end(), 0);
                    gs.write_register(ParallelGS::RegisterAddr::HWREG, word(uploadTail.data()));
                    uploadTailSize = 0;
                }
            }
        }

        void Flush() override
        {
            Lock lock(mutex);
            gs.flush();
        }

        void TextureFlush() override
        {
            WriteRegisterRaw(GS_REG_TEXFLUSH, 0);
        }

        void Sync(GSSyncReason reason) override
        {
            Lock lock(mutex);
            if (reason == GSSyncReason::Reset)
            {
                gs.flush();
                vulkan.device.wait_idle();
            }
            // VRAM maps and the transfer FIFO wait for their own readbacks.
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            Lock lock(mutex);
            traceCall('P', 0, reinterpret_cast<const uint8_t *>(&request), sizeof(request));
            gs.flush();
            auto &p = gs.get_priv_register_state();
            p.qwords_lo[0] = request.pmode;
            p.qwords_lo[2] = priv.smode1;
            p.qwords_lo[4] = request.smode2;
            p.qwords_lo[12] = priv.syncv;
            p.qwords_lo[14] = request.dispfb1;
            p.qwords_lo[16] = request.display1;
            p.qwords_lo[18] = request.dispfb2;
            p.qwords_lo[20] = request.display2;
            p.qwords_lo[28] = request.bgcolor;

            ParallelGS::VSyncInfo info{};
            info.phase = uint32_t((priv.csr.load() >> 13) & 1);
            info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
            info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            info.crtc_offsets = true;
            // Player setting: whole frames without field blending. paraLLEl-GS's high-resolution
            // scan-out is not offered: Burnout copies its frame through post-processing passes, and
            // the doubled scan-out came out duplicated side by side.
            info.force_progressive = ps2x::hostSettings().progressiveScan;
            info.anti_blur = ps2x::hostSettings().progressiveScan;
            if (std::getenv("BDR_PGS_TRACE"))
                std::printf("[pgs-present] pmode=%llx smode1=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bg=%llx\n",
                            (unsigned long long)request.pmode, (unsigned long long)priv.smode1, (unsigned long long)request.smode2,
                            (unsigned long long)request.dispfb1, (unsigned long long)request.display1,
                            (unsigned long long)request.dispfb2, (unsigned long long)request.display2, (unsigned long long)request.bgcolor);
            if (const char *dir = std::getenv("BDR_PGS_VRAM_PPM"))
            {
                // The DISPFB1 buffer as read back from paraLLEl-GS VRAM, 640x512.
                static unsigned index = 0;
                const uint32_t fbp = uint32_t(request.dispfb1 & 0x1ff), fbw = uint32_t((request.dispfb1 >> 9) & 0x3f);
                const unsigned psm = unsigned((request.dispfb1 >> 15) & 0x1f);
                auto *mapped = const_cast<uint8_t *>(static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize)));
                char path[512];
                std::snprintf(path, sizeof(path), "%s/vram_%04u.ppm", dir, index++);
                if (FILE *file = std::fopen(path, "wb"))
                {
                    std::fprintf(file, "P6\n640 512\n255\n");
                    for (uint32_t y = 0; y < 512; ++y)
                        for (uint32_t x = 0; x < 640; ++x)
                        {
                            uint32_t v = pixelAccess(psm).read(mapped, fbp * 32, fbw, x, y);
                            if (bitsPerPixel(psm) == 16)
                                v = ((v & 0x1f) << 3) | (((v >> 5) & 0x1f) << 11) | (((v >> 10) & 0x1f) << 19);
                            const unsigned char rgb[3] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)};
                            std::fwrite(rgb, 1, 3, file);
                        }
                    std::fclose(file);
                }
            }
            const ParallelGS::ScanoutResult scanout = gs.vsync(info);
            if (!scanout.image || scanout.image->get_format() != VK_FORMAT_R8G8B8A8_UNORM)
                return {};

            const uint32_t width = scanout.image->get_width();
            const uint32_t height = scanout.image->get_height();
            const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
            auto &device = vulkan.device;
            if (!readback || readback->get_create_info().size != bytes)
            {
                Vulkan::BufferCreateInfo info{};
                info.domain = Vulkan::BufferDomain::CachedHost;
                info.size = bytes;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                readback = device.create_buffer(info);
            }

            auto cmd = device.request_command_buffer();
            cmd->copy_image_to_buffer(*readback, *scanout.image, 0, {0, 0, 0}, {width, height, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            Vulkan::Fence fence;
            device.submit(cmd, &fence);
            fence->wait();

            PresentationFrame frame;
            frame.width = width;
            frame.height = height;
            frame.pixels.resize(size_t(bytes));
            const auto *mapped = static_cast<const uint8_t *>(device.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT));
            std::memcpy(frame.pixels.data(), mapped, frame.pixels.size());
            device.unmap_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT);
            for (size_t i = 3; i < frame.pixels.size(); i += 4)
                frame.pixels[i] = 0xff; // Scanout alpha is not a display value.
            return frame;
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            Lock lock(mutex);
            auto *mapped = const_cast<uint8_t *>(static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize)));
            return pixelAccess(psm).read(mapped, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            Lock lock(mutex);
            gs.map_vram_read(0, VramSize); // Keeps untouched pixels and packed-format lanes.
            auto *mapped = static_cast<uint8_t *>(gs.map_vram_write(0, VramSize));
            pixelAccess(psm).write(mapped, base, bw, x, y, value);
            gs.end_vram_write(0, VramSize);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            Lock lock(mutex);
            auto *mapped = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            out.assign(mapped, mapped + VramSize);
        }

        bool ClearFramebuffer(const GSContext &, uint32_t) override
        {
            return false; // The frontend does not hold the context registers of a raw backend.
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            Lock lock(mutex);
            if (!dst)
                return 0;
            auto count = uint32_t(std::min<size_t>(maxBytes, transfer.localToHostPendingBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                if (fifoOffset == 16)
                {
                    gs.read_transfer_fifo(fifoTail.data(), 1);
                    fifoOffset = 0;
                }
                dst[i] = fifoTail[fifoOffset++];
            }
            transfer.localToHostPendingBytes -= count;
            return count;
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            Lock lock(mutex);
            return transfer;
        }

        void SerializeState(ps2x::StateArchive &archive) override
        {
            Lock lock(mutex);
            archive.tag("PGS.");
            std::vector<uint8_t> vram;
            if (!archive.loading())
            {
                const auto *mapped = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
                vram.assign(mapped, mapped + VramSize);
            }
            archive.vector(vram);
            if (archive.loading() && vram.size() != VramSize)
                throw std::runtime_error("save state has a different GS VRAM size");
            archive.raw(&gs.get_register_state(), sizeof(ParallelGS::RegisterState));
            archive.raw(&gs.get_priv_register_state(), sizeof(ParallelGS::PrivRegisterState));
            for (uint32_t path = 0; path < 4; ++path)
                archive.raw(&gs.get_gif_path(path), sizeof(ParallelGS::GIFPath));
            archive.value(bitblt);
            archive.value(trxreg);
            archive.value(transfer);
            archive.value(transferBytes);
            archive.value(uploadedBytes);
            archive.value(uploadTail);
            archive.value(uploadTailSize);
            archive.value(fifoTail);
            archive.value(fifoOffset);
            if (archive.loading())
            {
                std::memcpy(gs.map_vram_write(0, VramSize), vram.data(), VramSize);
                gs.end_vram_write(0, VramSize);
                gs.write_register(ParallelGS::RegisterAddr::TEXFLUSH, uint64_t(0));
                gs.clobber_register_state();
            }
        }
    };
}

std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers)
{
    return std::make_unique<ParallelBackend>(registers);
}
