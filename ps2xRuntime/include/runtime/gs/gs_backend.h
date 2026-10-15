#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

namespace ps2x
{
    class StateArchive;
}

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    // A backend that decodes GIF packets and GS registers itself (paraLLEl-GS)
    // receives them unparsed; the frontend then skips its own decoding.
    virtual bool UsesRawCommands() const { return false; }
    virtual void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t sizeBytes) { (void)path, (void)data, (void)sizeBytes; }
    virtual void WriteRegisterRaw(uint8_t addr, uint64_t value) { (void)addr, (void)value; }

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
    // Save states: the whole backend state, VRAM included. Called with the GIF paths idle.
    virtual void SerializeState(ps2x::StateArchive &archive);
};
