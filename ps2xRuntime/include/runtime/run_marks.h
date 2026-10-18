#pragma once

#include <cstdint>

namespace ps2x
{
// F9 marks a moment of a windowed run. The window reports it with notifyMark, and a listener
// that keeps recent history (the game runner's camera watch) prints that history to the log.
using MarkListener = void (*)(uint64_t vsync);
void setMarkListener(MarkListener listener);
void notifyMark(uint64_t vsync);
}
