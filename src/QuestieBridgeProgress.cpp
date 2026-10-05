/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QuestieBridgeProgress.h"
#include "AreaDefines.h"
#include "WorldState.h"
#include "WorldStateDefines.h"
#include "WorldStatePackets.h"

void AppendQuestieBridgeProgress(std::vector<std::string>& rows)
{
    // UI values come from AC's dedicated progress structures, not getWorldState().
    // Constructing this packet does not send/change client UI.
    WorldPackets::WorldState::InitWorldStates packet;
    sWorldState->FillInitialWorldStates(packet, AREA_ISLE_OF_QUEL_DANAS, 0);
    bool scourgeActive = false;
    for (auto const& state : packet.Worldstates)
    {
        if (state.VariableID == WORLD_STATE_SCOURGE_INVASION_VICTORIES)
            scourgeActive = true;
        rows.push_back("U:" + std::to_string(state.VariableID) + ':' + std::to_string(state.Value));
    }
    rows.push_back(scourgeActive ? "P:SC_ACTIVE:1" : "P:SC_ACTIVE:0");
}
