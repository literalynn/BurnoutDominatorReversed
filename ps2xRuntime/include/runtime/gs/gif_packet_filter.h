#pragma once

#include <cstdint>

namespace ps2x
{
// A game's host-side hook on the GIF packets that reach the GS (Burnout: button prompts).
// It returns data unchanged, or a packet of its own (valid until its next call) and its size.
// A call with path 0 and no data says that GS memory and the GIF paths were reset or reloaded.
using GifPacketFilter = const uint8_t *(*)(uint32_t path, const uint8_t *data, uint32_t &size);
void setGifPacketFilter(GifPacketFilter filter);
// GS::reset and loading a save state call it.
void resetGifPacketFilter();
}
