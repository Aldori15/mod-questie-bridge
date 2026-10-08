/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_PHASES_H
#define QUESTIE_BRIDGE_PHASES_H

#include "Player.h"

#include <iterator>
#include <string>
#include <vector>

namespace QuestieBridge
{
// Keep these map/zone and map/area pairs in sync with QuestieServer.lua.
// Area pairs cover Acherus on map 0 and exports without a Shadow Vault zone ID.
constexpr uint32 PhaseZones[][2] =
{
    {0, 85}, {0, 1497}, {1, 1637},
    {571, 65}, {571, 66}, {571, 67}, {571, 210}, {571, 394}, {571, 3537}, {609, 4298}
};
constexpr uint32 PhaseAreas[][2] =
{
    {0, 4281}, {571, 4477}
};
constexpr uint32 PhaseMasks[] =
{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 19, 32, 35, 51, 64, 65, 66, 71,
    128, 129, 131, 175, 192, 193, 194, 195, 196, 197, 198, 204, 231, 243, 255, 256, 257,
    384, 448, 449, 510, 511, 65535, 2147483647, 4294967295
};
constexpr std::size_t PhaseMaxRows = 1 + std::size(PhaseMasks);

inline bool IsSupportedPhaseContext(uint32 map, uint32 area, uint32 zone)
{
    if (!area)
        return false;
    for (auto const& pair : PhaseAreas)
        if (pair[0] == map && pair[1] == area)
            return true;
    for (auto const& pair : PhaseZones)
        if (pair[0] == map && pair[1] == zone)
            return true;
    return false;
}

inline void AppendPhases(Player* player, std::vector<std::string>& rows)
{
    uint32 const map = player->GetMapId();
    uint32 const area = player->GetAreaId();
    uint32 const zone = player->GetZoneId();
    rows.push_back("P:PHASE_CONTEXT:" + std::to_string(map) + ':' + std::to_string(area)
        + ':' + std::to_string(player->GetPhaseMask()) + ':' + std::to_string(zone));
    // Phase bits are reused. Only interpret these decisions for coordinates in
    // this exact map and subarea; remote markers retain their usual locations.
    if (!IsSupportedPhaseContext(map, area, zone))
        return;
    for (uint32 mask : PhaseMasks)
        rows.push_back("F:" + std::to_string(area) + ':' + std::to_string(mask)
            + (player->InSamePhase(mask) ? ":1" : ":0"));
}
}

#endif
