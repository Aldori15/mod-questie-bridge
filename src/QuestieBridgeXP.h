/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_XP_H
#define QUESTIE_BRIDGE_XP_H

#include "Player.h"
#include "World.h"

#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace QuestieBridge
{
constexpr float QuestXPMaxMultiplier = 1000.0f;
static_assert(sizeof(float) == sizeof(uint32) && std::numeric_limits<float>::is_iec559);

inline bool AppendQuestXP(Player* player, std::vector<std::string>& rows)
{
    // The getter includes OnPlayerGetQuestRate overrides. Read per character;
    // reward/mutation hooks must never be invoked to produce a forecast.
    float const normal = player->GetQuestRate(false);
    float const dungeonFinder = player->GetQuestRate(true);
    float const aura = player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_QUEST_PCT);
    uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    for (float value : {normal, dungeonFinder, aura})
        if (!std::isfinite(value) || value < 0.0f || value > QuestXPMaxMultiplier)
            return false;
    if (!maxLevel || maxLevel > 255)
        return false;

    // Decimal float formatting loses precision near XP truncation boundaries.
    // Unsigned IEEE-754 bits preserve the core values exactly and avoid locale.
    auto bits = [](float value)
    {
        // Canonicalize negative zero to the client's unsigned positive encoding.
        return std::to_string(std::bit_cast<uint32>(value == 0.0f ? 0.0f : value));
    };
    rows.push_back("P:QUEST_XP:" + bits(normal) + ':' + bits(dungeonFinder)
        + ':' + bits(aura) + ':' + std::to_string(maxLevel));
    return true;
}
}

#endif
