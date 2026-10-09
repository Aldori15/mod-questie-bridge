/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_MONEY_H
#define QUESTIE_BRIDGE_MONEY_H

#include "World.h"

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace QuestieBridge
{
constexpr float QuestMoneyMaxMultiplier = 1000.0f;
static_assert(sizeof(float) == sizeof(uint32) && std::numeric_limits<float>::is_iec559);

inline bool AppendQuestMoney(std::vector<std::string>& rows)
{
    float const normal = sWorld->getRate(RATE_REWARD_QUEST_MONEY);
    float const bonus = sWorld->getRate(RATE_REWARD_BONUS_MONEY);
    uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    if (!std::isfinite(normal) || normal < 0.0f || normal > QuestMoneyMaxMultiplier
        || !std::isfinite(bonus) || bonus < 0.0f || bonus > QuestMoneyMaxMultiplier
        || !maxLevel || maxLevel > 255)
        return false;

    auto bits = [](float value)
    {
        return std::to_string(std::bit_cast<uint32>(value == 0.0f ? 0.0f : value));
    };
    rows.push_back("P:QUEST_MONEY:" + bits(normal) + ':' + bits(bonus) + ':' + std::to_string(maxLevel));
    return true;
}
}

#endif
