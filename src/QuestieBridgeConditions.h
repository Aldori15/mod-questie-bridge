/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_CONDITIONS_H
#define QUESTIE_BRIDGE_CONDITIONS_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace QuestieBridge
{
constexpr std::size_t QuestConditionsMaxQuests = 512;
constexpr std::size_t QuestConditionsMaxNodes = 32;
constexpr std::size_t QuestConditionsMaxDepth = 8;

// Own signatures and IDs only: .reload conditions frees every Condition object.
struct QuestConditionIndex
{
    std::map<std::string, std::vector<std::uint32_t>> Groups;
    std::size_t Quests = 0;
};

template <typename Quests, typename Conditions>
std::map<std::uint32_t, bool> FindQuestConditionCandidates(Quests const& quests, Conditions const& conditions)
{
    std::map<std::uint32_t, bool> candidates;
    for (auto const& [id, quest] : quests)
        if (id && quest && !conditions(id).empty())
            candidates.emplace(id, true);
    return candidates;
}

template <typename Conditions, typename References>
std::optional<std::string> QuestConditionSignature(Conditions const& conditions, References const& references,
    std::size_t& nodes, std::size_t depth = 0)
{
    if (conditions.empty() || depth >= QuestConditionsMaxDepth)
        return std::nullopt;
    std::map<std::uint32_t, std::vector<std::string>> groups;
    for (auto const* condition : conditions)
    {
        if (!condition || ++nodes > QuestConditionsMaxNodes || condition->ScriptId
            || condition->ConditionTarget || condition->SourceGroup || condition->SourceId)
            return std::nullopt;
        std::string signature;
        if (condition->ReferenceId)
        {
            // AC does not apply NegativeCondition to reference rows. Do not infer it.
            if (condition->NegativeCondition)
                return std::nullopt;
            auto const nested = QuestConditionSignature(references(condition->ReferenceId),
                references, nodes, depth + 1);
            if (!nested)
                return std::nullopt;
            signature = '(' + *nested + ')';
        }
        else
        {
            auto const type = static_cast<std::uint32_t>(condition->ConditionType);
            if (condition->ConditionValue3 || !condition->ConditionValue1
                || (type != 8 && type != 27 && type != 47)
                || (type == 8 && condition->ConditionValue2)
                || (type == 27 && (condition->ConditionValue1 > 255 || condition->ConditionValue2 > 4)))
                return std::nullopt;
            signature = std::to_string(type) + ':' + std::to_string(condition->ConditionValue1)
                + ':' + std::to_string(condition->ConditionValue2) + ':'
                + (condition->NegativeCondition ? "1" : "0");
        }
        groups[condition->ElseGroup].push_back(std::move(signature));
    }
    std::vector<std::string> alternatives;
    for (auto& [id, predicates] : groups)
    {
        (void)id;
        std::sort(predicates.begin(), predicates.end());
        std::string group = "[";
        for (auto const& predicate : predicates)
            group += predicate + ',';
        alternatives.push_back(group + ']');
    }
    std::sort(alternatives.begin(), alternatives.end());
    std::string signature;
    for (auto const& group : alternatives)
        signature += group + '|';
    return signature;
}

template <typename Quests, typename Conditions, typename References>
std::optional<QuestConditionIndex> BuildQuestConditionIndex(Quests const& quests,
    Conditions const& conditions, References const& references)
{
    QuestConditionIndex index;
    for (auto const& [id, quest] : quests)
    {
        if (!id || !quest)
            continue;
        std::size_t nodes = 0;
        auto const signature = QuestConditionSignature(conditions(id), references, nodes);
        if (!signature)
            continue;
        if (++index.Quests > QuestConditionsMaxQuests)
            return std::nullopt;
        index.Groups[*signature].push_back(id);
    }
    return index;
}

template <typename Evaluate>
bool AppendQuestConditions(QuestConditionIndex const& index, Evaluate const& evaluate,
    std::vector<std::string>& rows, std::string& caps, std::size_t reserved)
{
    std::vector<std::uint32_t> blocked;
    for (auto const& [signature, quests] : index.Groups)
    {
        (void)signature;
        if (!evaluate(quests.front()))
            blocked.insert(blocked.end(), quests.begin(), quests.end());
    }
    if (rows.size() + blocked.size() + 1 + reserved > 4096)
        return false;
    std::sort(blocked.begin(), blocked.end());
    caps += (caps.empty() ? "" : ",") + std::string("QUESTCONDITIONS");
    rows.push_back("P:QC_COUNT:" + std::to_string(blocked.size()));
    for (auto id : blocked)
        rows.push_back("C:" + std::to_string(id));
    return true;
}
}

#endif
