/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PoolMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldState.h"
#include "QuestieBridgeKaluak.h"
#include "QuestieBridgeICC.h"
#include "QuestieBridgePhases.h"
#include "QuestieBridgeProgress.h"
#include "QuestieBridgeWintergrasp.h"
#include "QuestieBridgeXP.h"
#include "QuestieBridgeReputation.h"
#include "QuestieBridgeMoney.h"
#include "QuestieBridgeStateCache.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr char Envelope[] = "QSTSVR\t";
constexpr uint32 ProtocolVersion = 15;
constexpr char ModuleVersion[] = "0.1.0";
constexpr uint32 Events = 1;
constexpr uint32 Values = 2;
constexpr uint32 Progress = 4;
constexpr uint32 Kaluak = 8;
constexpr uint32 QuestPools = 16;
constexpr uint32 Wintergrasp = 32;
constexpr uint32 ICC = 64;
constexpr uint32 Resets = 128;
constexpr uint32 Phases = 256;
constexpr uint32 Patrols = 512;
constexpr uint32 QuestXP = 1024;
constexpr uint32 QuestReputation = 2048;
constexpr uint32 QuestMoney = 4096;
constexpr std::size_t PhaseMaxRows = QuestieBridge::PhaseMaxRows;
std::atomic<uint32> Capabilities{Events | Values | Progress | Kaluak | QuestPools | Wintergrasp | ICC
    | Resets | Phases | Patrols | QuestXP | QuestReputation | QuestMoney};
std::atomic<bool> Dirty{false};
using Clock = std::chrono::steady_clock;

struct Subscriber
{
    std::string Token;
    std::vector<uint32> WorldStates;
    Clock::time_point LastRequest;
    Clock::time_point LastSend;
    Clock::time_point LastStateCheck;
    uint64 CommonRevision = 0;
    uint64 Sequence = 0;
    uint64 SnapshotSequence = 0;
    uint64 PatrolSequence = 0;
    uint64 PatrolSnapshot = 0;
    std::string PatrolContext;
    bool ReplyRequested = false;
    std::string Previous;
};

std::mutex SubscriberMutex;
std::map<ObjectGuid, Subscriber> Subscribers;
std::map<ObjectGuid, Clock::time_point> LastDiagnosticReply;

std::vector<std::string> GetQuestPoolRows()
{
    // Every accepted quest pool member has a creature or gameobject starter.
    // Read loaded relations, then validate membership against PoolMgr. This also
    // handles custom pools and relation reloads without a static list or SQL reads.
    std::map<uint32, uint32> members;
    auto collect = [&members](PooledQuestRelation const& relations)
    {
        for (auto const& relation : relations)
            if (uint32 poolId = sPoolMgr->IsPartOfAPool<Quest>(relation.first))
                members[relation.first] = poolId;
    };
    collect(sPoolMgr->mQuestCreatureRelation);
    collect(sPoolMgr->mQuestGORelation);
    std::vector<std::string> rows;
    for (auto const& [questId, poolId] : members)
        rows.push_back("Q:" + std::to_string(questId) + ':' + std::to_string(poolId)
            + (sPoolMgr->IsSpawnedObject<Quest>(questId) ? ":1" : ":0"));
    return rows;
}

bool ParseHeader(std::string const& payload, std::string_view prefix, uint32& protocol,
    std::string& token, std::size_t& body)
{
    if (payload.size() > 240 || payload.compare(0, prefix.size(), prefix) != 0)
        return false;
    auto const versionEnd = payload.find('~', prefix.size());
    if (versionEnd == std::string::npos || versionEnd == prefix.size() || payload[prefix.size()] == '0')
        return false;
    auto const parsed = std::from_chars(payload.data() + prefix.size(), payload.data() + versionEnd, protocol);
    if (parsed.ec != std::errc() || parsed.ptr != payload.data() + versionEnd || protocol == 0 || protocol > 65535)
        return false;
    auto const tokenStart = versionEnd + 1;
    auto const separator = payload.find('~', tokenStart);
    if (separator == std::string::npos)
        return false;
    token = payload.substr(tokenStart, separator - tokenStart);
    if (token.empty() || token.size() > 32
        || token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
            != std::string::npos)
        return false;
    body = separator + 1;
    return true;
}

bool ParseRequest(std::string const& payload, std::size_t body, std::vector<uint32>& ids)
{
    // Decimal uint32 subscriptions only; no SQL, arbitrary commands or private player data.
    auto first = payload.data() + body;
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

bool ParseAcknowledgement(std::string const& payload, std::size_t body, uint64& sequence)
{
    auto const first = payload.data() + body;
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

void SendInfo(Player* player, std::string const& token, char const* status)
{
    std::string revision = GitRevision::GetHash();
    if (revision.empty() || revision.size() > 64
        || revision.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-")
            != std::string::npos)
        revision = "unknown";
    // INFO has its own fixed envelope so an incompatible state protocol can still
    // report the expected version. It never carries or renews quest availability.
    Send(player, "INFO~1~" + token + '~' + std::to_string(ProtocolVersion) + '~' + ModuleVersion
        + '~' + revision + '~' + status);
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
    std::string const header = "~" + std::to_string(ProtocolVersion) + '~' + subscriber.Token
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
    Send(player, "ALIVE~" + std::to_string(ProtocolVersion) + '~' + subscriber.Token
        + '~' + std::to_string(++subscriber.Sequence)
        + '~' + std::to_string(subscriber.SnapshotSequence));
}

QuestieBridge::CommonState BuildCommonState(uint32 capabilities)
{
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
    uint64 const weeklyReset = sWorldState->getWorldState(WORLD_STATE_CUSTOM_WEEKLY_QUEST_RESET_TIME);
    uint64 const monthlyReset = sWorldState->getWorldState(WORLD_STATE_CUSTOM_MONTHLY_QUEST_RESET_TIME);
    bool const reportResets = (capabilities & Resets) && weeklyReset && monthlyReset
        && weeklyReset <= std::numeric_limits<uint32>::max() && monthlyReset <= std::numeric_limits<uint32>::max();
    if (reportResets)
    {
        addCapability("RESETS");
        commonRows.push_back("P:QUEST_RESETS:" + std::to_string(weeklyReset) + ':' + std::to_string(monthlyReset));
    }
    if (capabilities & Progress)
    {
        addCapability("SCOURGE");
        addCapability("QUELDANAS");
        AppendQuestieBridgeProgress(commonRows);
    }
    addCapability("HEARTBEAT");
    if (capabilities & Patrols)
        addCapability("PATROLS");
    if (capabilities & Kaluak)
    {
        addCapability("KALUAK");
        commonRows.push_back("P:KA_FINISHED:" + GetQuestieBridgeKaluakFinished());
    }
    if (capabilities & Wintergrasp)
    {
        addCapability("WINTERGRASP");
        AppendQuestieBridgeWintergrasp(commonRows);
    }
    if (capabilities & QuestPools)
    {
        auto const rows = GetQuestPoolRows();
        // Reserve per-player worldstate and ICC rows. Never send a
        // partial pool catalog: omissions would make inactive choices unknown.
        std::size_t const reserved = 16 + (reportResets ? 1 : 0)
            + ((capabilities & ICC) ? QuestieBridgeICCMaxRows : 0)
            + ((capabilities & Phases) ? PhaseMaxRows : 0) + ((capabilities & QuestXP) ? 1 : 0)
            + ((capabilities & QuestReputation) ? QuestieBridge::ReputationMaxRows : 0)
            + ((capabilities & QuestMoney) ? 1 : 0);
        if (commonRows.size() + rows.size() + reserved <= 4096)
        {
            addCapability("QUESTPOOLS");
            commonRows.insert(commonRows.end(), rows.begin(), rows.end());
        }
    }
    auto reputationRows = (capabilities & QuestReputation)
        ? QuestieBridge::GetQuestReputationFactionRows() : std::nullopt;
    return {std::move(caps), std::move(commonRows), std::move(reputationRows), reportResets};
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
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestPools", true))
                caps |= QuestPools;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Wintergrasp", true))
                caps |= Wintergrasp;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.ICC", true))
                caps |= ICC;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Resets", true))
                caps |= Resets;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Phases", true))
                caps |= Phases;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Patrols", true))
                caps |= Patrols;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestXP", true))
                caps |= QuestXP;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestReputation", true))
                caps |= QuestReputation;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestMoney", true))
                caps |= QuestMoney;
        }
        _patrolCacheDirty.store(true);
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
            _commonState.Reset();
            return;
        }
        auto const now = Clock::now();
        // Resolve and prune before sampling shared state. These pointers live
        // only within this tick, while the subscriber map is locked.
        std::vector<std::pair<Player*, Subscriber*>> active;
        active.reserve(Subscribers.size());
        bool replyRequested = false;
        for (auto it = Subscribers.begin(); it != Subscribers.end();)
        {
            Player* player = ObjectAccessor::FindPlayer(it->first);
            if (!player || now - it->second.LastRequest > std::chrono::seconds(45))
            {
                it = Subscribers.erase(it);
                continue;
            }
            active.emplace_back(player, &it->second);
            replyRequested = replyRequested || it->second.ReplyRequested;
            ++it;
        }
        if (active.empty())
        {
            _commonState.Reset();
            return;
        }
        bool const dirty = Dirty.exchange(false);
        if (_patrolCacheDirty.exchange(false))
        {
            _patrolEntries.clear();
            _patrolCached = false;
        }
        _patrolMaps.clear();
        auto const& common = _commonState.Get(now, capabilities, dirty, replyRequested,
            [capabilities]() { return BuildCommonState(capabilities); });
        for (auto const& [player, entry] : active)
        {
            Subscriber& subscriber = *entry;
            // Polling also observes worldstate changes for which AC has no universal hook.
            if (_commonState.ShouldCheckPlayer(now, subscriber.LastStateCheck, subscriber.CommonRevision,
                dirty, subscriber.ReplyRequested, subscriber.Previous.empty()))
            {
                subscriber.LastStateCheck = now;
                subscriber.CommonRevision = _commonState.GetRevision();
                auto rows = common.Rows;
                if (capabilities & Values)
                    for (uint32 id : subscriber.WorldStates)
                        rows.push_back("W:" + std::to_string(id) + ':'
                            + std::to_string(sWorldState->getWorldState(id)));
                std::string playerCaps = common.Caps;
                if ((capabilities & Phases) && rows.size() + PhaseMaxRows + (common.ReportResets ? 1 : 0) <= 4096)
                {
                    playerCaps += (playerCaps.empty() ? "" : ",");
                    playerCaps += "PHASES";
                    QuestieBridge::AppendPhases(player, rows);
                }
                if ((capabilities & ICC)
                    && rows.size() + QuestieBridgeICCMaxRows + (common.ReportResets ? 1 : 0) <= 4096)
                {
                    playerCaps += (playerCaps.empty() ? "" : ",");
                    playerCaps += "ICC";
                    AppendQuestieBridgeICC(player, rows);
                }
                if ((capabilities & QuestXP) && rows.size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestXP(player, rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTXP");
                if (common.ReputationRows
                    && rows.size() + common.ReputationRows->size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestReputation(player, *common.ReputationRows, rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTREP");
                if ((capabilities & QuestMoney) && rows.size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestMoney(rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTMONEY");
                std::string signature = playerCaps;
                for (std::string const& row : rows)
                    signature += ';' + row;
                bool const changed = signature != subscriber.Previous;
                if (changed || subscriber.ReplyRequested)
                {
                    if (changed)
                    {
                        // Sample the clock only when sending a snapshot. It must not
                        // change the signature and turn every heartbeat into a full batch.
                        if (common.ReportResets)
                            rows.push_back("P:SERVER_TIME:" + std::to_string(GameTime::GetGameTime().count()));
                        SendSnapshot(player, subscriber, playerCaps, rows);
                        subscriber.Previous = std::move(signature);
                    }
                    else
                        SendHeartbeat(player, subscriber);
                    subscriber.LastSend = now;
                    subscriber.ReplyRequested = false;
                }
            }
            // Heartbeats and patrols retain their cadence even between state checks.
            if (subscriber.SnapshotSequence && now - subscriber.LastSend >= std::chrono::seconds(10))
            {
                SendHeartbeat(player, subscriber);
                subscriber.LastSend = now;
            }
            if ((capabilities & Patrols) && subscriber.SnapshotSequence)
                SendPatrol(player, subscriber);
        }
    }

private:
    // Cache relation entries on config reload, and loaded moving spawn identities
    // once per map/tick. Never retain Creature pointers or force grids to load.
    void SendPatrol(Player* player, Subscriber& subscriber)
    {
        if (!_patrolCached)
        {
            for (auto const& relation : *sObjectMgr->GetCreatureQuestRelationMap())
                _patrolEntries.insert(relation.first);
            for (auto const& relation : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
                _patrolEntries.insert(relation.first);
            _patrolCached = true;
        }
        auto const mapKey = std::make_pair(player->GetMapId(), player->GetInstanceId());
        auto const& store = player->GetMap()->GetCreatureBySpawnIdStore();
        auto found = _patrolMaps.find(mapKey);
        if (found == _patrolMaps.end())
        {
            std::set<ObjectGuid::LowType> spawns;
            for (auto const& [spawnId, creature] : store)
            {
                if (!_patrolEntries.contains(creature->GetEntry()))
                    continue;
                CreatureData const* data = creature->GetCreatureData();
                if (creature->GetTransport() || (data && data->movementType != IDLE_MOTION_TYPE)
                    || creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                    spawns.insert(spawnId);
            }
            found = _patrolMaps.emplace(mapKey, std::move(spawns)).first;
        }
        std::vector<std::string> rows;
        uint32 const zone = player->GetZoneId();
        bool overflow = false;
        for (auto const spawnId : found->second)
        {
            Creature* selected = nullptr;
            bool ambiguous = false;
            auto const range = store.equal_range(spawnId);
            for (auto it = range.first; it != range.second; ++it)
            {
                Creature* creature = it->second;
                if (!creature->IsInWorld() || !creature->IsAlive() || creature->GetZoneId() != zone
                    || !_patrolEntries.contains(creature->GetEntry()) || !player->CanSeeOrDetect(creature)
                    || creature->IsHostileTo(player))
                    continue;
                if (selected)
                    ambiguous = true;
                selected = creature;
            }
            if (!selected || ambiguous || !std::isfinite(selected->GetPositionX()) || !std::isfinite(selected->GetPositionY()))
                continue;
            // Transport passengers already have world coordinates here, including
            // their deck offset. Do not project the database's deck-local position.
            rows.push_back(Acore::StringFormat("{}:{}:{:.3f}:{:.3f}", selected->GetEntry(), spawnId,
                selected->GetPositionX(), selected->GetPositionY()));
            if (rows.size() > 64)
            {
                rows.clear();
                overflow = true;
                break;
            }
        }
        std::string const context = Acore::StringFormat("{}~{}~{}", mapKey.first, mapKey.second, zone);
        std::string const status = overflow ? "OVERFLOW" : "READY";
        std::string const state = context + '~' + status + (rows.empty() ? "~EMPTY" : "~LIVE");
        if (rows.empty() && subscriber.PatrolContext == state && subscriber.PatrolSnapshot == subscriber.SnapshotSequence)
            return;
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
        // Separate sequencing and complete batches prevent partial positions from
        // moving pins, and cannot renew the main quest-state snapshot's freshness.
        std::string const header = Acore::StringFormat("~{}~{}~{}~{}", ProtocolVersion, subscriber.Token,
            subscriber.SnapshotSequence, ++subscriber.PatrolSequence);
        Send(player, "MBEGIN" + header + '~' + context + '~' + std::to_string(rows.size())
            + '~' + std::to_string(parts.size()) + '~' + status);
        for (std::size_t index = 0; index < parts.size(); ++index)
            Send(player, "MPART" + header + '~' + std::to_string(index + 1) + '~' + parts[index]);
        Send(player, "MEND" + header);
        subscriber.PatrolContext = state;
        subscriber.PatrolSnapshot = subscriber.SnapshotSequence;
    }

    QuestieBridge::CommonStateCache _commonState;
    uint32 _elapsed = 0;
    std::atomic<bool> _patrolCacheDirty{true};
    bool _patrolCached = false;
    std::set<uint32> _patrolEntries;
    std::map<std::pair<uint32, uint32>, std::set<ObjectGuid::LowType>> _patrolMaps;
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
        if (!player || receiver != player || type != CHAT_MSG_WHISPER)
            return false;
        std::string token;
        uint64 acknowledged = 0;
        uint32 requestedProtocol = 0;
        std::size_t body = 0;
        std::vector<uint32> ids;
        std::string const payload = message.substr(sizeof(Envelope) - 1);
        bool const renewal = payload.compare(0, 4, "ACK~") == 0;
        if (!ParseHeader(payload, renewal ? "ACK~" : "WATCH~", requestedProtocol, token, body))
            return false;
        bool const compatible = requestedProtocol == ProtocolVersion;
        // An incompatible WATCH receives diagnostics only; its body is not interpreted.
        bool const valid = renewal ? compatible && ParseAcknowledgement(payload, body, acknowledged)
            : !compatible || ParseRequest(payload, body, ids);
        if (!valid)
            return false;
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        auto const now = Clock::now();
        auto const found = Subscribers.find(player->GetGUID());
        if (found != Subscribers.end() && now - found->second.LastRequest < std::chrono::seconds(2))
            return false;
        if (renewal)
        {
            if (!Capabilities.load() || found == Subscribers.end() || found->second.Token != token)
                return false;
            Subscriber& subscriber = found->second;
            subscriber.LastRequest = now;
            subscriber.ReplyRequested = true;
            if (acknowledged != subscriber.SnapshotSequence)
                subscriber.Previous.clear();
            return false;
        }
        auto const previousInfo = LastDiagnosticReply.find(player->GetGUID());
        if (previousInfo != LastDiagnosticReply.end() && now - previousInfo->second < std::chrono::seconds(2))
            return false;
        LastDiagnosticReply[player->GetGUID()] = now;
        if (!compatible || !Capabilities.load())
        {
            SendInfo(player, token, compatible ? "DISABLED" : "MISMATCH");
            return false;
        }
        Subscriber& subscriber = Subscribers[player->GetGUID()];
        subscriber.Token = std::move(token);
        subscriber.WorldStates = std::move(ids);
        subscriber.LastRequest = now;
        subscriber.Previous.clear();
        subscriber.SnapshotSequence = 0;
        subscriber.ReplyRequested = true;
        SendInfo(player, subscriber.Token, "READY");
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        Subscribers.erase(player->GetGUID());
        LastDiagnosticReply.erase(player->GetGUID());
    }
};
}

void AddSC_questie_bridge()
{
    RegisterQuestieBridgeKaluak();
    RegisterQuestieBridgeICC();
    new QuestieBridgeWorldScript();
    new QuestieBridgeEventScript();
    new QuestieBridgePlayerScript();
}
