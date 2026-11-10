/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_STATE_CACHE_H
#define QUESTIE_BRIDGE_STATE_CACHE_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace QuestieBridge
{
using StateClock = std::chrono::steady_clock;
constexpr auto StateCheckInterval = std::chrono::seconds(2);

template <typename Subscribers, typename Key>
auto NextSubscriber(Subscribers& subscribers, Key& cursor)
{
    auto next = subscribers.upper_bound(cursor);
    if (next == subscribers.end())
        next = subscribers.begin();
    if (next != subscribers.end())
        cursor = next->first;
    return next;
}

struct CommonState
{
    std::string Caps;
    std::vector<std::string> Rows;
    std::optional<std::vector<std::string>> ReputationRows;
    bool ReportResets = false;
    std::set<std::uint32_t> StarterEntries = {};
    std::set<std::uint32_t> PatrolEntries = {};
    std::set<std::uint32_t> RestrictedGiverTemplates = {};
    std::optional<std::vector<std::string>> GiverTemplateRows = std::nullopt;
    std::vector<std::string> LiveRows = {};

    bool AppendGiverTemplates(std::vector<std::string>& rows, std::string& caps, std::size_t reserved) const
    {
        if (!GiverTemplateRows || rows.size() + GiverTemplateRows->size() + reserved > 4096)
            return false;
        caps += (caps.empty() ? "" : ",") + std::string("QUESTGIVERTEMPLATES");
        rows.insert(rows.end(), GiverTemplateRows->begin(), GiverTemplateRows->end());
        return true;
    }

    bool operator==(CommonState const&) const = default;
};

template <typename Observer, typename Target>
bool IsQuestCreatureObservable(Observer const* player, Target const* creature)
{
    return player->HaveAtClient(creature) && player->CanSeeOrDetect(creature, false, true);
}

// The caller owns these pointers only during the current world update.
template <typename Target, typename Observer>
std::vector<Target*> CollectObservableQuestCreatures(Observer* player, std::set<std::uint32_t> const& entries)
{
    std::vector<Target*> creatures;
    auto const zone = player->GetZoneId();
    player->DoForAllVisibleWorldObjects([&](auto* object)
    {
        Target* creature = object->ToCreature();
        if (!creature || !creature->GetSpawnId() || !entries.contains(creature->GetEntry())
            || creature->GetMap() != player->GetMap()
            || !creature->IsInWorld() || !creature->IsAlive() || creature->GetZoneId() != zone
            || !IsQuestCreatureObservable(player, creature) || creature->IsHostileTo(player))
            return;
        creatures.push_back(creature);
    });
    return creatures;
}

// Own only serialized values. Players, NPCs and core-owned catalogs are sampled
// by the builder on the world thread and must never be retained here.
class CommonStateCache
{
public:
    template <typename Builder>
    CommonState const& Get(StateClock::time_point now, std::uint32_t capabilities,
        bool dirty, Builder&& builder)
    {
        if (!_state || capabilities != _capabilities || dirty
            || now - _lastRefresh >= StateCheckInterval)
        {
            auto state = std::forward<Builder>(builder)();
            if (!_state || _state->Caps != state.Caps || _state->Rows != state.Rows)
                ++_catalogRevision;
            if (!_state || capabilities != _capabilities || *_state != state)
                ++_revision;
            _state = std::move(state);
            _capabilities = capabilities;
            _lastRefresh = now;
        }
        return *_state;
    }

    bool ShouldCheckPlayer(StateClock::time_point now, StateClock::time_point lastCheck,
        std::uint64_t revision, bool dirty, bool replyRequested, bool missingSnapshot) const
    {
        return dirty || replyRequested || missingSnapshot || revision != _revision
            || now - lastCheck >= StateCheckInterval;
    }

    std::uint64_t GetRevision() const { return _revision; }
    std::uint64_t GetCatalogRevision() const { return _catalogRevision; }

    std::string BuildPlayerSignature(std::string const& caps, std::span<std::string const> personalRows) const
    {
        std::string signature = std::to_string(_revision) + ':' + caps;
        for (auto const& row : personalRows)
            signature += ';' + row;
        return signature;
    }

    bool CanRenewPlayer(StateClock::time_point now, StateClock::time_point lastCheck,
        std::uint64_t revision) const
    {
        return _state && revision == _revision && now - lastCheck < StateCheckInterval;
    }

    void Reset() { _state.reset(); }

private:
    std::optional<CommonState> _state;
    StateClock::time_point _lastRefresh{};
    std::uint32_t _capabilities = 0;
    std::uint64_t _revision = 0;
    std::uint64_t _catalogRevision = 0;
};
}

#endif
