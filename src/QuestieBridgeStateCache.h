/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_STATE_CACHE_H
#define QUESTIE_BRIDGE_STATE_CACHE_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace QuestieBridge
{
using StateClock = std::chrono::steady_clock;
constexpr auto StateCheckInterval = std::chrono::seconds(2);

struct CommonState
{
    std::string Caps;
    std::vector<std::string> Rows;
    std::optional<std::vector<std::string>> ReputationRows;
    bool ReportResets = false;

    bool operator==(CommonState const&) const = default;
};

// Own only serialized values. Players, NPCs and core-owned catalogs are sampled
// by the builder on the world thread and must never be retained here.
class CommonStateCache
{
public:
    template <typename Builder>
    CommonState const& Get(StateClock::time_point now, std::uint32_t capabilities,
        bool dirty, bool replyRequested, Builder&& builder)
    {
        if (!_state || capabilities != _capabilities || dirty || replyRequested
            || now - _lastRefresh >= StateCheckInterval)
        {
            auto state = std::forward<Builder>(builder)();
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
    void Reset() { _state.reset(); }

private:
    std::optional<CommonState> _state;
    StateClock::time_point _lastRefresh{};
    std::uint32_t _capabilities = 0;
    std::uint64_t _revision = 0;
};
}

#endif
