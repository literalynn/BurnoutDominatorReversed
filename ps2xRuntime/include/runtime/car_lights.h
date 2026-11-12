#pragma once

#include <cstdint>

namespace ps2x
{
    // settings.ini car_lights: brighter glows on Burnout's car lights. The game draws them as
    // corona sprites with a VU1 program, from 64-byte entries in the list it also fills with the
    // traffic signals and the track's lamps. The runner notes each entry the game's car light
    // code writes, and when the same bytes are unpacked into VU1 data memory their colour is
    // scaled there: guest RAM and every other entry stay as the game wrote them.
    void noteCarLight(const uint8_t *entry);
    void brightenCarLights(uint8_t *vu1Data, uint32_t qwordAddress, uint32_t qwordCount);
}
