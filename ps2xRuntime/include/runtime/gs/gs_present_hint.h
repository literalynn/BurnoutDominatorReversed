#pragma once

namespace ps2x
{
// Told by the runtime before each presentation: another presentation is already
// queued behind this one, so the GS backend should not wait for the GPU to show
// this frame (the picture is then one presentation late). Without a backlog the
// backend waits a little for this frame's readback, which keeps the display current.
void setPresentBacklog(bool backlog);
}
