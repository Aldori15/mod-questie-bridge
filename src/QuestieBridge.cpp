/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "Chat.h"
#include "Config.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldState.h"
#include "QuestieBridgeKaluak.h"
#include "QuestieBridgeProgress.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr char Envelope[] = "QSTSVR\t";
constexpr char RequestPrefix[] = "WATCH~";
constexpr uint32 Events = 1;
constexpr uint32 Values = 2;
constexpr uint32 Progress = 4;
constexpr uint32 Kaluak = 8;
std::atomic<uint32> Capabilities{Events | Values | Progress | Kaluak};
std::atomic<bool> Dirty{false};
using Clock = std::chrono::steady_clock;

struct Subscriber
{
    char Protocol = '2';
    std::string Token;
    std::vector<uint32> WorldStates;
    Clock::time_point LastRequest;
    Clock::time_point LastSend;
    uint64 Sequence = 0;
    uint64 SnapshotSequence = 0;
    bool ReplyRequested = false;
    std::string Previous;
};

std::mutex SubscriberMutex;
std::map<ObjectGuid, Subscriber> Subscribers;

bool ParseRequest(std::string const& payload, char& protocol, std::string& token, std::vector<uint32>& ids)
{
    if (payload.size() > 240 || payload.size() < 9
        || payload.compare(0, sizeof(RequestPrefix) - 1, RequestPrefix) != 0)
        return false;
    protocol = payload[sizeof(RequestPrefix) - 1];
    if ((protocol != '2' && protocol != '3' && protocol != '4') || payload[sizeof(RequestPrefix)] != '~')
        return false;
    auto const tokenStart = sizeof(RequestPrefix) + 1;
    auto const separator = payload.find('~', tokenStart);
    if (separator == std::string::npos)
        return false;
    token = payload.substr(tokenStart, separator - tokenStart);
    if (token.empty() || token.size() > 32
        || token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
            != std::string::npos)
        return false;

    // Decimal uint32 subscriptions only; no SQL, arbitrary commands or private player data.
    auto first = payload.data() + separator + 1;
    auto const end = payload.data() + payload.size();
    while (first != end)
    {
        uint32 id = 0;
        auto const result = std::from_chars(first, end, id);
        if (result.ec != std::errc() || result.ptr == first || ids.size() >= 16
            || (result.ptr != end && *result.ptr != ','))
            return false;
        for (uint32 previous : ids)
            if (previous == id)
                return false;
        ids.push_back(id);
        first = result.ptr;
        if (first != end && ++first == end)
            return false;
    }
    return true;
}

bool ParseAcknowledgement(std::string const& payload, std::string& token, uint64& sequence)
{
    constexpr char prefix[] = "ACK~4~";
    if (payload.size() > 240 || payload.compare(0, sizeof(prefix) - 1, prefix) != 0)
        return false;
    auto const separator = payload.find('~', sizeof(prefix) - 1);
    if (separator == std::string::npos)
        return false;
    token = payload.substr(sizeof(prefix) - 1, separator - (sizeof(prefix) - 1));
    if (token.empty() || token.size() > 32
        || token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
            != std::string::npos)
        return false;
    auto const first = payload.data() + separator + 1;
    auto const end = payload.data() + payload.size();
    auto const result = std::from_chars(first, end, sequence);
    // Match the client's exact integer range; never acknowledge an unreceived snapshot.
    return result.ec == std::errc() && result.ptr == end && sequence > 0 && sequence <= 9007199254740991ULL;
}

void Send(Player* player, std::string const& payload)
{
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
        std::string(Envelope) + payload);
    player->SendDirectMessage(&packet);
}

void SendSnapshot(Player* player, Subscriber& subscriber, std::string const& caps,
    std::vector<std::string> const& rows)
{
    std::vector<std::string> parts;
    std::string part;
    for (std::string const& row : rows)
    {
        if (!part.empty() && part.size() + row.size() + 1 > 150)
        {
            parts.push_back(part);
            part.clear();
        }
        if (!part.empty())
            part += ';';
        part += row;
    }
    if (!part.empty())
        parts.push_back(part);
    std::string const header = std::string("~") + subscriber.Protocol + '~' + subscriber.Token
        + '~' + std::to_string(++subscriber.Sequence);
    subscriber.SnapshotSequence = subscriber.Sequence;
    Send(player, "BEGIN" + header + '~' + caps + '~' + std::to_string(rows.size())
        + '~' + std::to_string(parts.size()));
    for (std::size_t index = 0; index < parts.size(); ++index)
        Send(player, "PART" + header + '~' + std::to_string(index + 1) + '~' + parts[index]);
    Send(player, "END" + header);
}

void SendHeartbeat(Player* player, Subscriber& subscriber)
{
    Send(player, "ALIVE~4~" + subscriber.Token + '~' + std::to_string(++subscriber.Sequence)
        + '~' + std::to_string(subscriber.SnapshotSequence));
}

class QuestieBridgeWorldScript final : public WorldScript
{
public:
    QuestieBridgeWorldScript() : WorldScript("QuestieBridgeWorldScript",
        {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE}) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        uint32 caps = 0;
        if (sConfigMgr->GetOption<bool>("QuestieBridge.Enabled", true))
        {
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Events", true))
                caps |= Events;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.WorldStates", true))
                caps |= Values;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Progress", true))
                caps |= Progress;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Kaluak", true))
                caps |= Kaluak;
        }
        Capabilities.store(caps);
        Dirty.store(true);
        LOG_INFO("server.loading", "mod-questie-bridge: capability mask {}", caps);
    }

    void OnUpdate(uint32 diff) override
    {
        _elapsed += diff;
        if (_elapsed < 1000)
            return;
        _elapsed = 0;
        // Read core state and resolve players on the world thread, after event transitions finish.
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        uint32 const capabilities = Capabilities.load();
        if (!capabilities)
        {
            Subscribers.clear();
            return;
        }
        if (Subscribers.empty())
            return;
        bool const dirty = Dirty.exchange(false);
        auto const now = Clock::now();
        std::string caps;
        std::vector<std::string> commonRows;
        if (capabilities & Events)
        {
            caps = "EVENTS";
            auto const& events = sGameEventMgr->GetEventMap();
            for (std::size_t id = 1; id < events.size() && id <= std::numeric_limits<uint16>::max(); ++id)
            {
                auto const& event = events[id];
                if (!event.isValid())
                    continue;
                bool const mainStage = event.HolidayId != HOLIDAY_NONE
                    && event.HolidayStage == sGameEventMgr->GetHolidayMainStage(event.HolidayId);
                commonRows.push_back("E:" + std::to_string(id) + ':' + std::to_string(event.HolidayId)
                    + (mainStage ? ":1:" : ":0:") + (sGameEventMgr->IsActiveEvent(uint16(id)) ? "1" : "0"));
                // Never advertise a partial catalog as authoritative. Leave room for
                // progress/subscriptions within the client's bounded snapshot limits.
                if (commonRows.size() > 4000)
                {
                    caps.clear();
                    commonRows.clear();
                    break;
                }
            }
        }
        auto addCapability = [&caps](std::string const& value)
        {
            if (!caps.empty())
                caps += ',';
            caps += value;
        };
        if (capabilities & Values)
            addCapability("VALUES");
        if (capabilities & Progress)
        {
            addCapability("SCOURGE");
            addCapability("QUELDANAS");
            AppendQuestieBridgeProgress(commonRows);
        }
        std::string const kaluak = (capabilities & Kaluak) ? GetQuestieBridgeKaluakFinished() : "?";
        for (auto it = Subscribers.begin(); it != Subscribers.end();)
        {
            Subscriber& subscriber = it->second;
            Player* player = ObjectAccessor::FindPlayer(it->first);
            if (!player || now - subscriber.LastRequest > std::chrono::seconds(45))
            {
                it = Subscribers.erase(it);
                continue;
            }
            // Polling also observes worldstate changes for which AC has no universal hook.
            if (dirty || subscriber.ReplyRequested || now - subscriber.LastSend >= std::chrono::seconds(2)
                || subscriber.Previous.empty())
            {
                auto rows = commonRows;
                auto subscriberCaps = caps;
                // Advertise only features understood by the requested protocol.
                if (subscriber.Protocol == '4')
                {
                    if (!subscriberCaps.empty())
                        subscriberCaps += ',';
                    subscriberCaps += "HEARTBEAT";
                }
                if ((capabilities & Kaluak) && subscriber.Protocol != '2')
                {
                    if (!subscriberCaps.empty())
                        subscriberCaps += ',';
                    subscriberCaps += "KALUAK";
                    rows.push_back("P:KA_FINISHED:" + kaluak);
                }
                if (capabilities & Values)
                    for (uint32 id : subscriber.WorldStates)
                        rows.push_back("W:" + std::to_string(id) + ':'
                            + std::to_string(sWorldState->getWorldState(id)));
                std::string signature = subscriberCaps;
                for (std::string const& row : rows)
                    signature += ';' + row;
                bool const changed = signature != subscriber.Previous;
                if (changed || subscriber.ReplyRequested || now - subscriber.LastSend >= std::chrono::seconds(10))
                {
                    if (changed || subscriber.Protocol != '4')
                    {
                        SendSnapshot(player, subscriber, subscriberCaps, rows);
                        subscriber.Previous = std::move(signature);
                    }
                    else
                        SendHeartbeat(player, subscriber);
                    subscriber.LastSend = now;
                    subscriber.ReplyRequested = false;
                }
            }
            ++it;
        }
    }

private:
    uint32 _elapsed = 0;
};

class QuestieBridgeEventScript final : public GameEventScript
{
public:
    QuestieBridgeEventScript() : GameEventScript("QuestieBridgeEventScript",
        {GAMEEVENTHOOK_ON_START, GAMEEVENTHOOK_ON_STOP}) { }
    void OnStart(uint16 /*eventId*/) override { Dirty.store(true); }
    void OnStop(uint16 /*eventId*/) override { Dirty.store(true); }
};

class QuestieBridgePlayerScript final : public PlayerScript
{
public:
    QuestieBridgePlayerScript() : PlayerScript("QuestieBridgePlayerScript",
        {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_LOGOUT}) { }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Player* receiver) override
    {
        if (language != LANG_ADDON || message.compare(0, sizeof(Envelope) - 1, Envelope) != 0)
            return true;
        if (!Capabilities.load() || !player || receiver != player || type != CHAT_MSG_WHISPER)
            return false;
        std::string token;
        char protocol = '2';
        uint64 acknowledged = 0;
        std::vector<uint32> ids;
        std::string const payload = message.substr(sizeof(Envelope) - 1);
        bool const renewal = payload.compare(0, 4, "ACK~") == 0;
        bool const valid = renewal ? ParseAcknowledgement(payload, token, acknowledged)
            : ParseRequest(payload, protocol, token, ids);
        if (!valid)
            return false;
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        auto const now = Clock::now();
        auto const found = Subscribers.find(player->GetGUID());
        if (found != Subscribers.end() && now - found->second.LastRequest < std::chrono::seconds(2))
            return false;
        if (renewal)
        {
            if (found == Subscribers.end() || found->second.Protocol != '4' || found->second.Token != token)
                return false;
            Subscriber& subscriber = found->second;
            subscriber.LastRequest = now;
            subscriber.ReplyRequested = true;
            if (acknowledged != subscriber.SnapshotSequence)
                subscriber.Previous.clear();
            return false;
        }
        Subscriber& subscriber = Subscribers[player->GetGUID()];
        subscriber.Protocol = protocol;
        subscriber.Token = std::move(token);
        subscriber.WorldStates = std::move(ids);
        subscriber.LastRequest = now;
        subscriber.Previous.clear();
        subscriber.SnapshotSequence = 0;
        subscriber.ReplyRequested = true;
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        Subscribers.erase(player->GetGUID());
    }
};
}

void AddSC_questie_bridge()
{
    RegisterQuestieBridgeKaluak();
    new QuestieBridgeWorldScript();
    new QuestieBridgeEventScript();
    new QuestieBridgePlayerScript();
}
