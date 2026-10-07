/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QuestieBridgeICC.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <map>
#include <mutex>

namespace
{
constexpr uint32 IcecrownCitadel = 631;
// Public InstanceScript slots in IcecrownCitadel/icecrown_citadel.h.
constexpr uint32 DataWeeklyQuestId = 252;
constexpr uint32 DataValithriaDreamwalker = 10;
constexpr uint32 RespiteFamily = 24872;

struct QuestRule
{
    uint32 QuestId;
    uint32 Family;
    bool TwentyFive;
    TeamId Team;
};

// WeeklyQuestData uses the 10-player Horde ID as the saved family key for every mode.
// The lieutenant is changed to its Alliance entry by OnCreatureCreate when appropriate.
constexpr QuestRule QuestRules[] =
{
    {24869, 24869, false, TEAM_NEUTRAL},
    {24875, 24869, true, TEAM_NEUTRAL},
    {24870, 24870, false, TEAM_HORDE},
    {24871, 24870, false, TEAM_ALLIANCE},
    {24877, 24870, true, TEAM_HORDE},
    {24876, 24870, true, TEAM_ALLIANCE},
    {24872, RespiteFamily, false, TEAM_NEUTRAL},
    {24880, RespiteFamily, true, TEAM_NEUTRAL},
    {24873, 24873, false, TEAM_NEUTRAL},
    {24878, 24873, true, TEAM_NEUTRAL},
    {24874, 24874, false, TEAM_NEUTRAL},
    {24879, 24874, true, TEAM_NEUTRAL},
};
static_assert(sizeof(QuestRules) / sizeof(QuestRule) + 1 <= QuestieBridgeICCMaxRows);

struct InstanceState
{
    uint8 Difficulty;
    uint32 Family;
    bool RespiteReady;
    TeamId Team;
};

std::mutex StateMutex;
std::map<uint32, InstanceState> Observations;

class QuestieBridgeICCMapScript final : public AllMapScript
{
public:
    QuestieBridgeICCMapScript() : AllMapScript("QuestieBridgeICCMapScript",
        {ALLMAPHOOK_ON_CREATE_MAP, ALLMAPHOOK_ON_DESTROY_MAP, ALLMAPHOOK_ON_MAP_UPDATE}) { }

    void OnMapUpdate(Map* map, uint32 /*diff*/) override
    {
        if (map->GetId() != IcecrownCitadel || !map->GetInstanceId() || !map->IsDungeon())
            return;
        auto const* instanceMap = map->ToInstanceMap();
        auto const* script = instanceMap->GetInstanceScript();
        if (!script || instanceMap->GetScriptName() != "instance_icecrown_citadel"
            || map->GetSpawnMode() > RAID_DIFFICULTY_25MAN_HEROIC)
        {
            Forget(map);
            return;
        }
        uint32 const family = script->GetData(DataWeeklyQuestId);
        bool supported = family == 0;
        for (QuestRule const& rule : QuestRules)
            supported = supported || rule.Family == family;
        if (!supported || script->GetTeamIdInInstance() > TEAM_HORDE)
        {
            Forget(map);
            return;
        }
        // InstanceScript data is read on its own map update thread. Never retain the script pointer.
        InstanceState const state{map->GetSpawnMode(), family,
            script->GetBossState(DataValithriaDreamwalker) == DONE, script->GetTeamIdInInstance()};
        std::lock_guard<std::mutex> guard(StateMutex);
        Observations[map->GetInstanceId()] = state;
    }

    void OnCreateMap(Map* map) override { Forget(map); }
    void OnDestroyMap(Map* map) override { Forget(map); }

private:
    static void Forget(Map const* map)
    {
        if (map->GetId() != IcecrownCitadel)
            return;
        std::lock_guard<std::mutex> guard(StateMutex);
        Observations.erase(map->GetInstanceId());
    }
};
}

void AppendQuestieBridgeICC(Player const* player, std::vector<std::string>& rows)
{
    if (player->GetMapId() != IcecrownCitadel || !player->GetInstanceId())
    {
        rows.emplace_back("P:ICC_STATE:0:0:0:0:0");
        return;
    }
    std::lock_guard<std::mutex> guard(StateMutex);
    auto const found = Observations.find(player->GetInstanceId());
    if (found == Observations.end())
    {
        rows.emplace_back("P:ICC_STATE:?:?:?:?:?");
        return;
    }
    InstanceState const& state = found->second;
    rows.push_back("P:ICC_STATE:" + std::to_string(player->GetInstanceId()) + ':'
        + std::to_string(state.Difficulty) + ':' + std::to_string(state.Family)
        + (state.RespiteReady ? ":1:" : ":0:") + std::to_string(state.Team));
    bool const twentyFive = (state.Difficulty & RAID_DIFFICULTY_MASK_25MAN) != 0;
    for (QuestRule const& rule : QuestRules)
    {
        bool const active = rule.Family == state.Family && rule.TwentyFive == twentyFive
            && (rule.Team == TEAM_NEUTRAL || rule.Team == state.Team)
            && (rule.Family != RespiteFamily || state.RespiteReady);
        rows.push_back("I:" + std::to_string(rule.QuestId) + (active ? ":1" : ":0"));
    }
}

void RegisterQuestieBridgeICC()
{
    new QuestieBridgeICCMapScript();
}
