/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_REPUTATION_H
#define QUESTIE_BRIDGE_REPUTATION_H

#include "DBCStores.h"
#include "Formulas.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"

#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace QuestieBridge
{
constexpr std::size_t ReputationMaxFactions = 256;
constexpr std::size_t ReputationMaxRows = ReputationMaxFactions + 1;
constexpr float ReputationMaxMultiplier = 1000.0f;
static_assert(sizeof(float) == sizeof(uint32) && std::numeric_limits<float>::is_iec559);

inline bool ValidReputationMultiplier(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= ReputationMaxMultiplier;
}

inline std::string ReputationMultiplierBits(float value)
{
    return std::to_string(std::bit_cast<uint32>(value == 0.0f ? 0.0f : value));
}

inline std::optional<std::vector<std::string>> GetQuestReputationFactionRows()
{
    // Read AC's loaded rates, including .reload reputation_reward_rate changes.
    // No SQL or per-quest queries; share this complete catalog across subscribers.
    std::vector<std::string> rows;
    for (uint32 id = 1; id < sFactionStore.GetNumRows(); ++id)
    {
        if (!sFactionStore.LookupEntry(id))
            continue;
        RepRewardRate const* rates = sObjectMgr->GetRepRewardRate(id);
        if (!rates)
            continue;
        if (rows.size() >= ReputationMaxFactions)
            return std::nullopt;
        std::string row = "T:" + std::to_string(id);
        for (float value : {rates->questRate, rates->questDailyRate, rates->questWeeklyRate,
            rates->questMonthlyRate, rates->questRepeatableRate})
        {
            if (!ValidReputationMultiplier(value))
                return std::nullopt;
            row += ':' + ReputationMultiplierBits(value);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

inline bool AppendQuestReputation(Player* player, std::vector<std::string> const& factionRows,
    std::vector<std::string>& rows)
{
    float const gain = sWorld->getRate(RATE_REPUTATION_GAIN);
    float const lowLevel = sWorld->getRate(RATE_REPUTATION_LOWLEVEL_QUEST);
    int32 const aura = player->GetTotalAuraModifier(SPELL_AURA_MOD_REPUTATION_GAIN);
    float const recruitAFriend = player->GetsRecruitAFriendBonus(false)
        ? 1.0f + sWorld->getRate(RATE_REPUTATION_RECRUIT_A_FRIEND_BONUS) : 1.0f;
    if (!ValidReputationMultiplier(gain) || !ValidReputationMultiplier(lowLevel)
        || !ValidReputationMultiplier(recruitAFriend) || aura < -10000 || aura > 10000)
        return false;

    // Report state only. Never invoke reputation reward/change hooks to preview.
    rows.push_back("P:QUEST_REP:" + ReputationMultiplierBits(gain) + ':' + ReputationMultiplierBits(lowLevel)
        + ':' + std::to_string(aura) + ':' + ReputationMultiplierBits(recruitAFriend)
        + ':' + std::to_string(Acore::XP::GetGrayLevel(player->GetLevel()))
        + ':' + std::to_string(factionRows.size()));
    rows.insert(rows.end(), factionRows.begin(), factionRows.end());
    return true;
}
}

#endif
