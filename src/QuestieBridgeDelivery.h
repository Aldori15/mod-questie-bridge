/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_DELIVERY_H
#define QUESTIE_BRIDGE_DELIVERY_H

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace QuestieBridge
{
class SnapshotDelivery
{
public:
    using Clock = std::chrono::steady_clock;

    SnapshotDelivery(std::vector<std::string> messages, Clock::time_point now)
        : _messages(std::move(messages)), _created(now) { }

    bool Expired(Clock::time_point now) const { return now - _created >= std::chrono::seconds(4); }
    bool CanContinue(Clock::time_point now, std::string_view queuedCatalog, std::string_view currentCatalog) const
    {
        return !Expired(now) && queuedCatalog == currentCatalog;
    }
    bool Complete() const { return _next == _messages.size(); }
    std::size_t Remaining() const { return _messages.size() - _next; }

    template <typename Sender>
    std::size_t Dispatch(std::size_t allowance, Sender&& send)
    {
        std::size_t sent = 0;
        while (!Complete() && sent < allowance)
        {
            send(_messages[_next++]);
            ++sent;
        }
        return sent;
    }

private:
    std::vector<std::string> _messages;
    Clock::time_point _created;
    std::size_t _next = 0;
};
}

#endif
