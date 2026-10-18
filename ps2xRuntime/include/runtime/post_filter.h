#pragma once

#include "ps2_host_backend.h"

namespace ps2x
{
    // The frame to draw into the window: the game's frame, or its FXAA version rendered at the
    // frame's own size (a render texture, stored upside down). Call before BeginDrawing.
    struct PostFrame
    {
        Texture2D texture;
        bool flipped;
    };
    PostFrame applyPostFilter(const Texture2D &frame, bool fxaa, bool smooth);
}
