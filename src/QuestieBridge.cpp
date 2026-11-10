/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "Chat.h"
#include "Config.h"
#include "ConditionMgr.h"
#include "CryptoHash.h"
#include "Creature.h"
#include "DatabaseScript.h"
#include "DisableMgr.h"
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
#include "QuestieBridgeProtocol.h"
#include "QuestieBridgeConditions.h"
#include "QuestieBridgeDelivery.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
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
using QuestieBridge::ProtocolVersion;
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
constexpr uint32 QuestDisables = 8192;
constexpr uint32 Questgivers = 16384;
constexpr uint32 QuestgiverTemplates = 32768;
constexpr uint32 QuestConditions = 65536;
constexpr std::size_t PhaseMaxRows = QuestieBridge::PhaseMaxRows;
std::atomic<uint32> Capabilities{Events | Values | Progress | Kaluak | QuestPools | Wintergrasp | ICC
    | Resets | Phases | Patrols | QuestXP | QuestReputation | QuestMoney
    | QuestDisables | Questgivers | QuestgiverTemplates};
std::atomic<bool> Dirty{false};
std::atomic<bool> EventsChanged{false};
using Clock = std::chrono::steady_clock;

struct PendingSnapshot
{
    QuestieBridge::SnapshotDelivery Delivery;
    std::string Signature;
    std::string Catalog;
    uint64 Sequence;
};

struct Subscriber
{
    std::string Token;
    std::vector<uint32> WorldStates;
    Clock::time_point LastRequest;
    Clock::time_point LastSend;
    Clock::time_point LastStateCheck;
    Clock::time_point LastPatrolCheck;
    Clock::time_point RetryAfter;
    uint64 CommonRevision = 0;
    uint64 Sequence = 0;
    uint64 SnapshotSequence = 0;
    uint64 PatrolSequence = 0;
    uint64 PatrolSnapshot = 0;
    std::string PatrolContext;
    bool ReplyRequested = false;
    bool RenewalRequested = false;
    std::string Previous;
    std::string KnownCatalog;
    std::optional<PendingSnapshot> Pending;
};

std::mutex SubscriberMutex;
std::map<ObjectGuid, Subscriber> Subscribers;
std::deque<ObjectGuid> PendingSnapshots;
std::size_t PendingMessages = 0;
std::map<ObjectGuid, Clock::time_point> LastDiagnosticReply;

void CancelSnapshot(ObjectGuid guid, Subscriber& subscriber)
{
    if (subscriber.Pending)
        PendingMessages -= subscriber.Pending->Delivery.Remaining();
    subscriber.Pending.reset();
    std::erase(PendingSnapshots, guid);
}

struct Timing
{
    uint64 Calls = 0;
    uint64 Microseconds = 0;
    uint64 Maximum = 0;
};

class Measure
{
public:
    explicit Measure(Timing* timing) : _timing(timing), _start(timing ? Clock::now() : Clock::time_point{}) { }
    ~Measure()
    {
        if (!_timing)
            return;
        auto const elapsed = uint64(std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - _start).count());
        ++_timing->Calls;
        _timing->Microseconds += elapsed;
        _timing->Maximum = std::max(_timing->Maximum, elapsed);
    }

private:
    Timing* _timing;
    Clock::time_point _start;
};

struct Performance
{
    Timing Updates;
    Timing Common;
    Timing Players;
    Timing Conditions;
    Timing Sampling;
    Timing PatrolChecks;
    Timing Encoding;
    Timing Dispatch;
    uint64 Visible = 0;
    uint64 Packets = 0;
    uint64 Bytes = 0;
    uint64 Full = 0;
    uint64 Reused = 0;
    uint64 Heartbeats = 0;
    uint64 Patrols = 0;
    uint64 Deferred = 0;
    uint64 Cancelled = 0;
};

std::vector<std::string> GetQuestPoolRows(std::vector<std::string>& selections)
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
    std::vector<std::string> active;
    for (auto const& [questId, poolId] : members)
    {
        rows.push_back("Q:" + std::to_string(questId) + ':' + std::to_string(poolId));
        if (sPoolMgr->IsSpawnedObject<Quest>(questId))
            active.push_back("S:" + std::to_string(questId));
    }
    selections.push_back("P:POOL_COUNT:" + std::to_string(active.size()));
    selections.insert(selections.end(), active.begin(), active.end());
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

bool ParseRequest(std::string const& payload, std::size_t body, std::vector<uint32>& ids, std::string& catalog)
{
    auto const separator = payload.find('~', body);
    if (separator == std::string::npos)
        return false;
    catalog = payload.substr(separator + 1);
    if (catalog != "-" && !QuestieBridge::ValidCatalogId(catalog))
        return false;
    // Decimal uint32 subscriptions only; no SQL, arbitrary commands or private player data.
    auto first = payload.data() + body;
    auto const end = payload.data() + separator;
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

std::string BuildHeartbeat(Subscriber& subscriber)
{
    return "ALIVE~" + std::to_string(ProtocolVersion) + '~' + subscriber.Token
        + '~' + std::to_string(++subscriber.Sequence)
        + '~' + std::to_string(subscriber.SnapshotSequence);
}

QuestieBridge::CommonState BuildCommonState(uint32 capabilities)
{
    std::string caps;
    std::vector<std::string> commonRows;
    std::vector<std::string> liveRows;
    if (capabilities & Events)
    {
        caps = "EVENTS";
        std::vector<std::string> active;
        auto const& events = sGameEventMgr->GetEventMap();
        for (std::size_t id = 1; id < events.size() && id <= std::numeric_limits<uint16>::max(); ++id)
        {
            auto const& event = events[id];
            if (!event.isValid())
                continue;
            bool const mainStage = event.HolidayId != HOLIDAY_NONE
                && event.HolidayStage == sGameEventMgr->GetHolidayMainStage(event.HolidayId);
            commonRows.push_back("E:" + std::to_string(id) + ':' + std::to_string(event.HolidayId)
                + (mainStage ? ":1" : ":0"));
            if (sGameEventMgr->IsActiveEvent(uint16(id)))
                active.push_back("A:" + std::to_string(id));
            // Never advertise a partial catalog as authoritative. Leave room for
            // progress/subscriptions within the client's bounded snapshot limits.
            if (commonRows.size() + active.size() + 1 > 4000)
            {
                caps.clear();
                commonRows.clear();
                break;
            }
        }
        if (!caps.empty())
        {
            liveRows.push_back("P:EVENT_COUNT:" + std::to_string(active.size()));
            liveRows.insert(liveRows.end(), active.begin(), active.end());
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
        AppendQuestieBridgeProgress(liveRows);
    }
    addCapability("HEARTBEAT");
    if (capabilities & Patrols)
        addCapability("PATROLS");
    if (capabilities & Kaluak)
    {
        addCapability("KALUAK");
        liveRows.push_back("P:KA_FINISHED:" + GetQuestieBridgeKaluakFinished());
    }
    if (capabilities & Wintergrasp)
    {
        addCapability("WINTERGRASP");
        AppendQuestieBridgeWintergrasp(liveRows);
    }
    if (capabilities & QuestPools)
    {
        std::vector<std::string> selections;
        auto const rows = GetQuestPoolRows(selections);
        // Reserve per-player worldstate and ICC rows. Never send a
        // partial pool catalog: omissions would make inactive choices unknown.
        std::size_t const reserved = 16 + (reportResets ? 1 : 0)
            + ((capabilities & ICC) ? QuestieBridgeICCMaxRows : 0)
            + ((capabilities & Phases) ? PhaseMaxRows : 0) + ((capabilities & QuestXP) ? 1 : 0)
            + ((capabilities & QuestReputation) ? QuestieBridge::ReputationMaxRows : 0)
            + ((capabilities & QuestMoney) ? 1 : 0);
        if (commonRows.size() + liveRows.size() + rows.size() + selections.size() + reserved <= 4096)
        {
            addCapability("QUESTPOOLS");
            commonRows.insert(commonRows.end(), rows.begin(), rows.end());
            liveRows.insert(liveRows.end(), selections.begin(), selections.end());
        }
    }

    if (capabilities & QuestDisables)
    {
        std::vector<uint32> disabled;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
            if (quest && sDisableMgr->IsDisabledFor(DISABLE_TYPE_QUEST, questId, nullptr))
                disabled.push_back(questId);

        std::size_t const reserved = 16 + (reportResets ? 1 : 0)
            + ((capabilities & ICC) ? QuestieBridgeICCMaxRows : 0)
            + ((capabilities & Phases) ? PhaseMaxRows : 0) + ((capabilities & QuestXP) ? 1 : 0)
            + ((capabilities & QuestReputation) ? QuestieBridge::ReputationMaxRows : 0)
            + ((capabilities & QuestMoney) ? 1 : 0);

        if (commonRows.size() + liveRows.size() + disabled.size() + reserved <= 4096)
        {
            std::sort(disabled.begin(), disabled.end());
            addCapability("QUESTDISABLES");
            for (uint32 questId : disabled)
                commonRows.push_back("D:" + std::to_string(questId));
        }
    }

    auto reputationRows = (capabilities & QuestReputation)
        ? QuestieBridge::GetQuestReputationFactionRows() : std::nullopt;
    QuestieBridge::CommonState common{std::move(caps), std::move(commonRows), std::move(reputationRows), reportResets};
    common.LiveRows = std::move(liveRows);
    if (capabilities & (Questgivers | QuestgiverTemplates | Patrols))
    {
        for (auto const& relation : *sObjectMgr->GetCreatureQuestRelationMap())
            common.StarterEntries.insert(relation.first);
        for (auto const& relation : sPoolMgr->mQuestCreatureRelation)
            common.StarterEntries.insert(relation.second);
    }
    if (capabilities & Patrols)
    {
        common.PatrolEntries = common.StarterEntries;
        for (auto const& relation : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            common.PatrolEntries.insert(relation.first);
    }
    if (capabilities & QuestgiverTemplates)
    {
        for (uint32 entry : common.StarterEntries)
            if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry))
                if (!(info->npcflag & UNIT_NPC_FLAG_QUESTGIVER))
                    common.RestrictedGiverTemplates.insert(entry);

        std::vector<std::string> rows;
        rows.push_back("P:QGT_COUNT:" + std::to_string(common.RestrictedGiverTemplates.size()));
        for (uint32 entry : common.RestrictedGiverTemplates)
            rows.push_back("N:" + std::to_string(entry));
        common.GiverTemplateRows = std::move(rows);
    }
    return common;
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
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestDisables", true))
                caps |= QuestDisables;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.Questgivers", true))
                caps |= Questgivers;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestgiverTemplates", true))
                caps |= QuestgiverTemplates;
            if (sConfigMgr->GetOption<bool>("QuestieBridge.QuestConditions", false))
                caps |= QuestConditions;
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
        Capabilities.store(caps);
        _maxWorkTime = std::chrono::milliseconds(std::clamp(
            sConfigMgr->GetOption<uint32>("QuestieBridge.MaxWorkTimeMs", 2), 1U, 10U));
        _maxSubscribers = std::clamp(
            sConfigMgr->GetOption<uint32>("QuestieBridge.MaxSubscribersPerUpdate", 64), 1U, 1024U);
        _maxMessages = std::clamp(
            sConfigMgr->GetOption<uint32>("QuestieBridge.MaxMessagesPerUpdate", 256), 128U, 4096U);
        _diagnostics = sConfigMgr->GetOption<bool>("QuestieBridge.PerformanceLogging", false);
        _performance = {};
        _lastReport = Clock::now();
        Dirty.store(true);
        LOG_INFO("server.loading", "mod-questie-bridge: capability mask {}", caps);
    }

    void OnUpdate(uint32 diff) override
    {
        _elapsed += diff;
        if (_elapsed < 100)
            return;
        _elapsed %= 100;
        // Read core state and resolve players on the world thread, after event transitions finish.
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        uint32 const capabilities = Capabilities.load();
        if (!capabilities)
        {
            Subscribers.clear();
            PendingSnapshots.clear();
            PendingMessages = 0;
            _commonState.Reset();
            _conditionCandidates.reset();
            return;
        }
        auto const now = Clock::now();
        if (Subscribers.empty())
        {
            _commonState.Reset();
            _conditionCandidates.reset();
            return;
        }
        bool const dirty = Dirty.exchange(false);
        if (dirty)
        {
            _conditionCandidates.reset();
            for (auto& [guid, subscriber] : Subscribers)
            {
                if (subscriber.Pending)
                {
                    CancelSnapshot(guid, subscriber);
                    subscriber.Previous.clear();
                }
                subscriber.CommonRevision = 0;
            }
        }
        bool const eventsChanged = EventsChanged.exchange(false);
        auto const& common = _commonState.Get(now, capabilities, dirty || eventsChanged, [this, capabilities]()
        {
            Measure measure(_diagnostics ? &_performance.Common : nullptr);
            return BuildCommonState(capabilities);
        });
        if (!_catalog || _catalogRevision != _commonState.GetCatalogRevision())
        {
            Measure measure(_diagnostics ? &_performance.Encoding : nullptr);
            Acore::Crypto::SHA256 hash;
            hash.UpdateData(common.Caps);
            for (auto const& row : common.Rows)
            {
                hash.UpdateData(";");
                hash.UpdateData(row);
            }
            hash.Finalize();
            std::string id;
            constexpr char digits[] = "0123456789ABCDEF";
            for (auto byte : hash.GetDigest())
            {
                id += digits[byte >> 4];
                id += digits[byte & 15];
            }
            _catalog = QuestieBridge::BuildSnapshotCatalog(std::move(id), common.Rows);
            _catalogRevision = _commonState.GetCatalogRevision();
        }
        if (!_catalog)
            return;
        auto const reservedMessages = PendingSnapshots.empty() ? 0 : _maxMessages / 2;
        std::size_t messages = _maxMessages - reservedMessages;
        std::optional<QuestieBridge::QuestConditionIndex> conditions;
        bool conditionsBuilt = false;
        auto const deadline = Clock::now() + _maxWorkTime;
        auto remaining = std::min<std::size_t>(_maxSubscribers, Subscribers.size());
        while (remaining && messages && !Subscribers.empty() && Clock::now() < deadline)
        {
            --remaining;
            auto const previousCursor = _cursor;
            auto const it = QuestieBridge::NextSubscriber(Subscribers, _cursor);
            Subscriber& subscriber = it->second;
            Player* player = ObjectAccessor::FindPlayer(it->first);
            if (!player || now - subscriber.LastRequest > std::chrono::seconds(45))
            {
                CancelSnapshot(it->first, subscriber);
                Subscribers.erase(it);
                continue;
            }
            if (subscriber.Pending)
                continue;
            // Sample once when needed; core-owned pointers never leave this player update.
            std::optional<std::vector<Creature*>> visibleCreatures;
            auto getVisibleCreatures = [&]() -> std::vector<Creature*> const&
            {
                if (!visibleCreatures)
                {
                    Measure measure(_diagnostics ? &_performance.Sampling : nullptr);
                    auto const& entries = (capabilities & Patrols) ? common.PatrolEntries : common.StarterEntries;
                    visibleCreatures = QuestieBridge::CollectObservableQuestCreatures<Creature>(player, entries);
                    if (_diagnostics)
                        _performance.Visible += visibleCreatures->size();
                }
                return *visibleCreatures;
            };
            // Polling also observes worldstate changes for which AC has no universal hook.
            bool const needsCheck = _commonState.ShouldCheckPlayer(now, subscriber.LastStateCheck,
                subscriber.CommonRevision, false, subscriber.ReplyRequested, subscriber.Previous.empty());
            if (needsCheck && now >= subscriber.RetryAfter)
            {
                Measure measure(_diagnostics ? &_performance.Players : nullptr);
                subscriber.LastStateCheck = now;
                subscriber.CommonRevision = _commonState.GetRevision();
                std::vector<std::string> rows = common.LiveRows;
                auto const sharedRows = common.Rows.size();
                if (capabilities & Values)
                    for (uint32 id : subscriber.WorldStates)
                        rows.push_back("W:" + std::to_string(id) + ':'
                            + std::to_string(sWorldState->getWorldState(id)));
                std::string playerCaps = common.Caps;
                if (capabilities & (Questgivers | QuestgiverTemplates))
                {
                    uint32 const zone = player->GetZoneId();
                    std::vector<std::string> observations;
                    std::map<std::pair<uint32, uint32>, std::vector<Creature*>> observed;
                    for (Creature* creature : getVisibleCreatures())
                        if (common.StarterEntries.contains(creature->GetEntry()))
                            observed[{creature->GetEntry(), creature->GetSpawnId()}].push_back(creature);
                    for (auto const& [identity, creatures] : observed)
                    {
                        bool const mixed = std::any_of(creatures.begin(), creatures.end(),
                            [](Creature* creature) { return creature->IsQuestGiver(); })
                            && std::any_of(creatures.begin(), creatures.end(),
                                [](Creature* creature) { return !creature->IsQuestGiver(); });
                        for (Creature* creature : creatures)
                        {
                            bool const active = creature->IsQuestGiver();
                            if ((!active && (capabilities & Questgivers))
                                || (active && (common.RestrictedGiverTemplates.contains(identity.first)
                                    || (mixed && (capabilities & Questgivers)))))
                                observations.push_back(Acore::StringFormat("G:{}:{}:{}:{:016X}",
                                    identity.first, identity.second, active ? 1 : 0,
                                    creature->GetGUID().GetRawValue()));
                        }
                        if (observations.size() > 64)
                            break;
                    }

                    std::size_t const reserved =
                        ((capabilities & Phases) ? PhaseMaxRows : 0)
                        + ((capabilities & ICC) ? QuestieBridgeICCMaxRows : 0)
                        + ((capabilities & QuestXP) ? 1 : 0)
                        + ((capabilities & QuestReputation) ? QuestieBridge::ReputationMaxRows : 0)
                        + ((capabilities & QuestMoney) ? 1 : 0)
                        + (common.ReportResets ? 1 : 0);

                    if (zone && observations.size() <= 64 && sharedRows + rows.size() + observations.size() + 1 + reserved <= 4096)
                    {
                        std::sort(observations.begin(), observations.end());
                        playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTGIVERS");
                        rows.push_back("P:QG_CONTEXT:" + std::to_string(player->GetMapId())
                            + ':' + std::to_string(zone));
                        rows.insert(rows.end(), observations.begin(), observations.end());
                        // Never apply template restrictions without a complete set of live exceptions.
                        common.AppendGiverTemplates(rows, playerCaps, sharedRows + reserved);
                    }
                }
                if ((capabilities & Phases) && sharedRows + rows.size() + PhaseMaxRows + (common.ReportResets ? 1 : 0) <= 4096)
                {
                    playerCaps += (playerCaps.empty() ? "" : ",");
                    playerCaps += "PHASES";
                    QuestieBridge::AppendPhases(player, rows);
                }
                if ((capabilities & ICC)
                    && sharedRows + rows.size() + QuestieBridgeICCMaxRows + (common.ReportResets ? 1 : 0) <= 4096)
                {
                    playerCaps += (playerCaps.empty() ? "" : ",");
                    playerCaps += "ICC";
                    AppendQuestieBridgeICC(player, rows);
                }
                if ((capabilities & QuestXP) && sharedRows + rows.size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestXP(player, rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTXP");
                if (common.ReputationRows
                    && sharedRows + rows.size() + common.ReputationRows->size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestReputation(player, *common.ReputationRows, rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTREP");
                if ((capabilities & QuestMoney) && sharedRows + rows.size() + 1 + (common.ReportResets ? 1 : 0) <= 4096
                    && QuestieBridge::AppendQuestMoney(rows))
                    playerCaps += (playerCaps.empty() ? "" : ",") + std::string("QUESTMONEY");
                if (capabilities & QuestConditions)
                {
                    Measure conditionMeasure(_diagnostics ? &_performance.Conditions : nullptr);
                    // Shared for this tick; never reuse classification across a condition reload.
                    if (!conditionsBuilt)
                    {
                        conditionsBuilt = true;
                        auto available = [](uint32 id)
                        {
                            return sConditionMgr->GetConditionsForNotGroupedEntry(
                                CONDITION_SOURCE_TYPE_QUEST_AVAILABLE, id);
                        };
                        if (!_conditionCandidates || now - _lastConditionDiscovery >= QuestieBridge::StateCheckInterval)
                        {
                            _conditionCandidates = QuestieBridge::FindQuestConditionCandidates(
                                sObjectMgr->GetQuestTemplates(), available);
                            _lastConditionDiscovery = now;
                        }
                        conditions = QuestieBridge::BuildQuestConditionIndex(*_conditionCandidates,
                            available, [](uint32 id) { return sConditionMgr->GetConditionReferences(id); });
                    }
                    if (conditions)
                        QuestieBridge::AppendQuestConditions(*conditions, [player](uint32 id)
                        {
                            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
                            return !quest || player->SatisfyQuestConditions(quest, false);
                        }, rows, playerCaps, sharedRows + (common.ReportResets ? 1 : 0));
                }
                std::string signature = std::to_string(_commonState.GetRevision()) + ':' + playerCaps;
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
                        if (!QueueSnapshot(player, subscriber, playerCaps, rows, std::move(signature), now))
                        {
                            subscriber.Previous.clear();
                            subscriber.RetryAfter = now + std::chrono::seconds(1);
                            continue;
                        }
                    }
                    else
                    {
                        subscriber.RenewalRequested = true;
                        subscriber.ReplyRequested = false;
                    }
                }
            }
            if (subscriber.Pending)
                continue;
            if (messages && subscriber.SnapshotSequence && !subscriber.Previous.empty()
                && _commonState.CanRenewPlayer(now, subscriber.LastStateCheck, subscriber.CommonRevision)
                && (subscriber.RenewalRequested || now - subscriber.LastSend >= std::chrono::seconds(10)))
            {
                SendMessage(player, BuildHeartbeat(subscriber));
                --messages;
                if (_diagnostics)
                    ++_performance.Heartbeats;
                subscriber.LastSend = now;
                subscriber.RenewalRequested = false;
            }
            if ((capabilities & Patrols) && subscriber.SnapshotSequence
                && now - subscriber.LastPatrolCheck >= std::chrono::seconds(1))
            {
                if (SendPatrol(player, subscriber, common, getVisibleCreatures(), messages))
                    subscriber.LastPatrolCheck = now;
                else
                {
                    _cursor = previousCursor;
                    break;
                }
            }
        }
        messages += reservedMessages;
        DispatchSnapshots(messages);
        ReportPerformance(now);
    }

private:
    void SendMessage(Player* player, std::string const& message)
    {
        Send(player, message);
        if (_diagnostics)
        {
            ++_performance.Packets;
            _performance.Bytes += message.size();
        }
    }

    bool QueueSnapshot(Player* player, Subscriber& subscriber, std::string const& caps,
        std::vector<std::string> const& rows, std::string signature, Clock::time_point now)
    {
        Measure measure(_diagnostics ? &_performance.Encoding : nullptr);
        if (PendingSnapshots.size() >= 64)
        {
            if (_diagnostics)
                ++_performance.Deferred;
            return false;
        }
        bool const reuse = subscriber.KnownCatalog == _catalog->Id;
        auto messages = QuestieBridge::BuildSnapshotMessages(subscriber.Token,
            subscriber.Sequence + 1, caps, rows, *_catalog, reuse);
        if (!messages)
            return false;
        // Keep queued work below three seconds of reserved dispatch capacity.
        if (PendingMessages + messages->size() > _maxMessages / 2 * 30)
        {
            if (_diagnostics)
                ++_performance.Deferred;
            return false;
        }
        PendingMessages += messages->size();
        subscriber.Pending.emplace(PendingSnapshot{QuestieBridge::SnapshotDelivery(std::move(*messages), now),
            std::move(signature), _catalog->Id, ++subscriber.Sequence});
        PendingSnapshots.push_back(player->GetGUID());
        if (_diagnostics)
            ++(reuse ? _performance.Reused : _performance.Full);
        return true;
    }

    void DispatchSnapshots(std::size_t& messages)
    {
        Measure measure(_diagnostics ? &_performance.Dispatch : nullptr);
        auto const now = Clock::now();
        while (messages && !PendingSnapshots.empty())
        {
            auto const guid = PendingSnapshots.front();
            auto const found = Subscribers.find(guid);
            if (found == Subscribers.end())
            {
                PendingSnapshots.pop_front();
                continue;
            }
            Subscriber& subscriber = found->second;
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || !subscriber.Pending || now - subscriber.LastRequest > std::chrono::seconds(45))
            {
                CancelSnapshot(guid, subscriber);
                continue;
            }
            auto& pending = *subscriber.Pending;
            if (!pending.Delivery.CanContinue(now, pending.Catalog, _catalog->Id))
            {
                CancelSnapshot(guid, subscriber);
                subscriber.Previous.clear();
                subscriber.ReplyRequested = true;
                if (_diagnostics)
                    ++_performance.Cancelled;
                continue;
            }
            auto const sent = pending.Delivery.Dispatch(messages,
                [this, player](std::string const& message) { SendMessage(player, message); });
            messages -= sent;
            PendingMessages -= sent;
            if (pending.Delivery.Complete())
            {
                subscriber.Previous = std::move(pending.Signature);
                subscriber.KnownCatalog = std::move(pending.Catalog);
                subscriber.SnapshotSequence = pending.Sequence;
                subscriber.LastSend = now;
                subscriber.ReplyRequested = false;
                subscriber.RenewalRequested = false;
                CancelSnapshot(guid, subscriber);
            }
        }
    }

    void ReportPerformance(Clock::time_point now)
    {
        if (!_diagnostics)
            return;
        auto const elapsed = uint64(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
        ++_performance.Updates.Calls;
        _performance.Updates.Microseconds += elapsed;
        _performance.Updates.Maximum = std::max(_performance.Updates.Maximum, elapsed);
        if (now - _lastReport < std::chrono::seconds(60))
            return;
        LOG_INFO("module.questiebridge", "Questie bridge: subscribers {}, queued {}, full/reused {}/{}, "
            "heartbeats {}, patrols {}, packets {}, payload bytes {}, deferred {}, cancelled {}",
            Subscribers.size(), PendingSnapshots.size(), _performance.Full, _performance.Reused,
            _performance.Heartbeats, _performance.Patrols, _performance.Packets, _performance.Bytes,
            _performance.Deferred, _performance.Cancelled);
        LOG_INFO("module.questiebridge", "Questie bridge timing (calls/total us/max us): "
            "updates {}/{}/{}, common {}/{}/{}, "
            "players {}/{}/{}, conditions {}/{}/{}, sampling {}/{}/{}, patrol checks {}/{}/{}, "
            "encoding {}/{}/{}, dispatch {}/{}/{}; visible NPCs {}",
            _performance.Updates.Calls, _performance.Updates.Microseconds, _performance.Updates.Maximum,
            _performance.Common.Calls, _performance.Common.Microseconds, _performance.Common.Maximum,
            _performance.Players.Calls, _performance.Players.Microseconds, _performance.Players.Maximum,
            _performance.Conditions.Calls, _performance.Conditions.Microseconds, _performance.Conditions.Maximum,
            _performance.Sampling.Calls, _performance.Sampling.Microseconds, _performance.Sampling.Maximum,
            _performance.PatrolChecks.Calls, _performance.PatrolChecks.Microseconds, _performance.PatrolChecks.Maximum,
            _performance.Encoding.Calls, _performance.Encoding.Microseconds, _performance.Encoding.Maximum,
            _performance.Dispatch.Calls, _performance.Dispatch.Microseconds, _performance.Dispatch.Maximum,
            _performance.Visible);
        _performance = {};
        _lastReport = now;
    }

    bool SendPatrol(Player* player, Subscriber& subscriber, QuestieBridge::CommonState const& common,
        std::vector<Creature*> const& visibleCreatures, std::size_t& messages)
    {
        Measure measure(_diagnostics ? &_performance.PatrolChecks : nullptr);
        auto const mapKey = std::make_pair(player->GetMapId(), player->GetInstanceId());
        auto const& store = player->GetMap()->GetCreatureBySpawnIdStore();
        std::vector<std::string> rows;
        uint32 const zone = player->GetZoneId();
        bool overflow = false;
        std::map<uint32, std::vector<Creature*>> observed;
        for (Creature* creature : visibleCreatures)
            if (common.PatrolEntries.contains(creature->GetEntry()))
                observed[creature->GetSpawnId()].push_back(creature);
        for (auto const& [spawnId, creatures] : observed)
        {
            if (creatures.size() != 1)
                continue;
            // A moving sibling can classify this spawn even when that sibling is invisible.
            auto const range = store.equal_range(spawnId);
            bool const patrol = std::any_of(range.first, range.second, [&common](auto const& item)
            {
                Creature* creature = item.second;
                if (!creature || !common.PatrolEntries.contains(creature->GetEntry()))
                    return false;
                CreatureData const* data = creature->GetCreatureData();
                return creature->GetTransport() || (data && data->movementType != IDLE_MOTION_TYPE)
                    || creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE;
            });
            Creature* selected = creatures.front();
            if (!patrol || !std::isfinite(selected->GetPositionX()) || !std::isfinite(selected->GetPositionY()))
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
            return true;
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
        if (parts.size() + 2 > messages)
        {
            if (_diagnostics)
                ++_performance.Deferred;
            return false;
        }
        messages -= parts.size() + 2;
        if (_diagnostics)
            ++_performance.Patrols;
        // Separate sequencing and complete batches prevent partial positions from
        // moving pins, and cannot renew the main quest-state snapshot's freshness.
        std::string const header = Acore::StringFormat("~{}~{}~{}~{}", ProtocolVersion, subscriber.Token,
            subscriber.SnapshotSequence, ++subscriber.PatrolSequence);
        SendMessage(player, "MBEGIN" + header + '~' + context + '~' + std::to_string(rows.size())
            + '~' + std::to_string(parts.size()) + '~' + status);
        for (std::size_t index = 0; index < parts.size(); ++index)
            SendMessage(player, "MPART" + header + '~' + std::to_string(index + 1) + '~' + parts[index]);
        SendMessage(player, "MEND" + header);
        subscriber.PatrolContext = state;
        subscriber.PatrolSnapshot = subscriber.SnapshotSequence;
        return true;
    }

    QuestieBridge::CommonStateCache _commonState;
    std::optional<QuestieBridge::SnapshotCatalog> _catalog;
    uint64 _catalogRevision = 0;
    std::optional<std::map<uint32, bool>> _conditionCandidates;
    Clock::time_point _lastConditionDiscovery;
    Clock::time_point _lastReport;
    ObjectGuid _cursor;
    std::chrono::milliseconds _maxWorkTime{2};
    std::size_t _maxSubscribers = 64;
    std::size_t _maxMessages = 256;
    bool _diagnostics = false;
    Performance _performance;
    uint32 _elapsed = 0;
};

class QuestieBridgeEventScript final : public GameEventScript
{
public:
    QuestieBridgeEventScript() : GameEventScript("QuestieBridgeEventScript",
        {GAMEEVENTHOOK_ON_START, GAMEEVENTHOOK_ON_STOP}) { }
    void OnStart(uint16 /*eventId*/) override { EventsChanged.store(true); }
    void OnStop(uint16 /*eventId*/) override { EventsChanged.store(true); }
};

class QuestieBridgeDatabaseScript final : public DatabaseScript
{
public:
    QuestieBridgeDatabaseScript() : DatabaseScript("QuestieBridgeDatabaseScript",
        {DATABASEHOOK_ON_AFTER_DATABASE_LOAD_CREATURETEMPLATES}) { }

    void OnAfterDatabaseLoadCreatureTemplates(std::vector<CreatureTemplate*> /*templates*/) override
    {
        Dirty.store(true);
    }
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
        std::string catalog;
        std::string const payload = message.substr(sizeof(Envelope) - 1);
        bool const renewal = payload.compare(0, 4, "ACK~") == 0;
        if (!ParseHeader(payload, renewal ? "ACK~" : "WATCH~", requestedProtocol, token, body))
            return false;
        bool const compatible = requestedProtocol == ProtocolVersion;
        // An incompatible WATCH receives diagnostics only; its body is not interpreted.
        bool const valid = renewal ? compatible && ParseAcknowledgement(payload, body, acknowledged)
            : !compatible || ParseRequest(payload, body, ids, catalog);
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
            subscriber.RenewalRequested = true;
            if (acknowledged != subscriber.SnapshotSequence)
            {
                CancelSnapshot(player->GetGUID(), subscriber);
                subscriber.ReplyRequested = true;
                subscriber.RetryAfter = {};
                subscriber.Previous.clear();
                subscriber.KnownCatalog.clear();
            }
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
        CancelSnapshot(player->GetGUID(), subscriber);
        subscriber.RetryAfter = {};
        subscriber.Token = std::move(token);
        subscriber.WorldStates = std::move(ids);
        subscriber.KnownCatalog = std::move(catalog);
        subscriber.LastRequest = now;
        subscriber.Previous.clear();
        subscriber.SnapshotSequence = 0;
        subscriber.ReplyRequested = true;
        subscriber.RenewalRequested = false;
        SendInfo(player, subscriber.Token, "READY");
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> guard(SubscriberMutex);
        auto const found = Subscribers.find(player->GetGUID());
        if (found != Subscribers.end())
            CancelSnapshot(player->GetGUID(), found->second);
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
    new QuestieBridgeDatabaseScript();
    new QuestieBridgePlayerScript();
}
