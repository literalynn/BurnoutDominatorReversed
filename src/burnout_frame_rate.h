#pragma once

class PS2Runtime;

namespace burnout {
// Wraps the game functions that set or assume its step rate (60 or 50 a second), so that it runs
// at the runtime's VBlank rate (runtime/vblank_rate.h) when one is set. Without one they only
// call the original.
void installFrameRate(PS2Runtime& runtime);
} // namespace burnout
