/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QuestieBridgeKaluak.h"
#include "AllCreatureScript.h"
#include "Creature.h"
#include "CreatureAI.h"

#include <map>
#include <mutex>

namespace
{
// npc_elder_clearwater in World/npcs_special.cpp exposes its in-memory winner flag
// through GetData(DATA_DERBY_FINISHED). It is not a persistent WorldState value.
constexpr uint32 ElderClearwater = 38294;
constexpr uint32 Northrend = 571;
constexpr uint32 DataDerbyFinished = 1;
std::mutex StateMutex;
std::map<ObjectGuid, bool> Observations;

bool IsClearwater(Creature const* creature)
{
    return creature->GetEntry() == ElderClearwater && creature->GetMapId() == Northrend
        && creature->GetInstanceId() == 0;
}

class QuestieBridgeKaluakScript final : public AllCreatureScript
{
public:
    QuestieBridgeKaluakScript() : AllCreatureScript("QuestieBridgeKaluakScript") { }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        if (!IsClearwater(creature))
            return;
        // Read AI only on this creature's update thread. The world thread receives
        // copied values, never a cross-thread Creature/AI pointer.
        bool const supported = creature->IsAlive() && creature->AI()
            && creature->GetScriptName() == "npc_elder_clearwater";
        uint32 const finished = supported ? creature->AI()->GetData(DataDerbyFinished) : 2;
        std::lock_guard<std::mutex> guard(StateMutex);
        if (finished <= 1)
            Observations[creature->GetGUID()] = finished != 0;
        else
            Observations.erase(creature->GetGUID());
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        Forget(creature);
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        Forget(creature);
    }

private:
    static void Forget(Creature* creature)
    {
        if (!IsClearwater(creature))
            return;
        std::lock_guard<std::mutex> guard(StateMutex);
        Observations.erase(creature->GetGUID());
    }
};
}

std::string GetQuestieBridgeKaluakFinished()
{
    std::lock_guard<std::mutex> guard(StateMutex);
    if (Observations.empty())
        return "?";
    bool const finished = Observations.begin()->second;
    for (auto const& observation : Observations)
        if (observation.second != finished)
            return "?";
    return finished ? "1" : "0";
}

void RegisterQuestieBridgeKaluak()
{
    new QuestieBridgeKaluakScript();
}
