/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QuestieBridgeWintergrasp.h"
#include "BattlefieldMgr.h"
#include "PoolMgr.h"

namespace
{
struct QuestRule
{
    uint32 QuestId;
    TeamId Team;
    bool Attacker;
    uint32 SelectionQuest;
};

// Match npc_wg_quest_giver in src/server/scripts/Northrend/zone_wintergrasp.cpp.
// SelectionQuest is the defending pool member that enables an attacking variant.
// These are scripted rules, not a replacement for PoolMgr's dynamic membership.
constexpr QuestRule QuestRules[] =
{
    {13199, TEAM_HORDE, true, 13193},
    {13202, TEAM_HORDE, true, 13192},
    {13180, TEAM_HORDE, true, 0},
    {13200, TEAM_HORDE, true, 13191},
    {13201, TEAM_HORDE, true, 13194},
    {13223, TEAM_HORDE, true, 0},
    {13193, TEAM_HORDE, false, 0},
    {13192, TEAM_HORDE, false, 0},
    {13178, TEAM_HORDE, false, 0},
    {13191, TEAM_HORDE, false, 0},
    {13194, TEAM_HORDE, false, 0},
    {13539, TEAM_HORDE, false, 0},
    {13185, TEAM_HORDE, false, 0},
    {13196, TEAM_ALLIANCE, true, 13154},
    {13198, TEAM_ALLIANCE, true, 13153},
    {13179, TEAM_ALLIANCE, true, 0},
    {13222, TEAM_ALLIANCE, true, 0},
    {13195, TEAM_ALLIANCE, true, 13156},
    {13197, TEAM_ALLIANCE, true, 236},
    {13154, TEAM_ALLIANCE, false, 0},
    {13153, TEAM_ALLIANCE, false, 0},
    {13177, TEAM_ALLIANCE, false, 0},
    {13538, TEAM_ALLIANCE, false, 0},
    {13186, TEAM_ALLIANCE, false, 0},
    {13156, TEAM_ALLIANCE, false, 0},
    {236, TEAM_ALLIANCE, false, 0},
};
}

void AppendQuestieBridgeWintergrasp(std::vector<std::string>& rows)
{
    Battlefield const* battlefield = sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG);
    if (!battlefield || battlefield->GetDefenderTeam() > TEAM_HORDE)
    {
        // No initialized battlefield means unknown, not a confirmed inactive quest.
        rows.emplace_back("P:WG_STATE:?:?:?");
        return;
    }
    rows.push_back(std::string("P:WG_STATE:") + (battlefield->IsEnabled() ? "1:" : "0:")
        + (battlefield->IsWarTime() ? "1:" : "0:") + std::to_string(battlefield->GetDefenderTeam()));
    for (QuestRule const& rule : QuestRules)
    {
        bool active = rule.Team == (rule.Attacker ? battlefield->GetAttackerTeam() : battlefield->GetDefenderTeam());
        if (rule.SelectionQuest)
            active = active && sPoolMgr->IsSpawnedObject<Quest>(rule.SelectionQuest);
        // Direct membership also matters if a server puts an attacking variant
        // in a custom pool. Preserve both gates even with QuestPools disabled.
        if (sPoolMgr->IsPartOfAPool<Quest>(rule.QuestId))
            active = active && sPoolMgr->IsSpawnedObject<Quest>(rule.QuestId);
        // The core script does not gate these offers on IsWarTime or IsEnabled.
        // Character eligibility remains the client's responsibility.
        rows.push_back("R:" + std::to_string(rule.QuestId) + (active ? ":1" : ":0"));
    }
}
