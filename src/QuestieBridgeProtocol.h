/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_PROTOCOL_H
#define QUESTIE_BRIDGE_PROTOCOL_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace QuestieBridge
{
constexpr std::uint32_t ProtocolVersion = 20;
using CapabilityMask = std::array<std::uint32_t, 4>;

struct WireCapability
{
    std::string_view Name;
    std::size_t Bit;
};

// Permanent wire assignments; these are not configuration flags.
constexpr std::array<WireCapability, 19> WireCapabilities = {{
    {"EVENTS", 0}, {"VALUES", 1}, {"SCOURGE", 2}, {"QUELDANAS", 3},
    {"KALUAK", 4}, {"HEARTBEAT", 5}, {"QUESTPOOLS", 6}, {"WINTERGRASP", 7},
    {"ICC", 8}, {"RESETS", 9}, {"PHASES", 10}, {"PATROLS", 11},
    {"QUESTXP", 12}, {"QUESTREP", 13}, {"QUESTMONEY", 14},
    {"QUESTDISABLES", 15}, {"QUESTGIVERS", 16}, {"QUESTGIVERTEMPLATES", 17}, {"QUESTCONDITIONS", 18}
}};

inline std::string EncodeCapabilityMask(CapabilityMask const& words)
{
    std::size_t count = words.size();
    while (count > 1 && !words[count - 1])
        --count;
    std::string result;
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index)
            result += '.';
        for (int shift = 28; shift >= 0; shift -= 4)
            result += digits[(words[index] >> shift) & 15];
    }
    return result;
}

inline std::optional<std::string> EncodeCapabilities(std::string_view names)
{
    CapabilityMask words{};
    while (!names.empty())
    {
        auto const separator = names.find(',');
        auto const name = names.substr(0, separator);
        bool found = false;
        for (auto const& capability : WireCapabilities)
            if (capability.Name == name)
            {
                auto& word = words[capability.Bit / 32];
                std::uint32_t const bit = std::uint32_t{1} << (capability.Bit % 32);
                if (word & bit)
                    return std::nullopt;
                word |= bit;
                found = true;
                break;
            }
        if (!found)
            return std::nullopt;
        if (separator == std::string_view::npos)
            break;
        names.remove_prefix(separator + 1);
        if (names.empty())
            return std::nullopt;
    }
    return EncodeCapabilityMask(words);
}

inline std::optional<std::vector<std::string>> BuildSnapshotMessages(std::string const& token,
    std::uint64_t sequence, std::string_view names, std::vector<std::string> const& rows)
{
    auto const caps = EncodeCapabilities(names);
    constexpr char tokenCharacters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (!caps || token.empty() || token.size() > 32
        || token.find_first_not_of(tokenCharacters) != std::string::npos
        || !sequence || sequence > 9007199254740991ULL || rows.size() > 4096)
        return std::nullopt;
    std::vector<std::string> parts;
    std::string part;
    for (auto const& row : rows)
    {
        if (row.empty() || row.size() > 150)
            return std::nullopt;
        if (!part.empty() && part.size() + row.size() + 1 > 150)
        {
            parts.push_back(std::move(part));
            part.clear();
        }
        if (!part.empty())
            part += ';';
        part += row;
    }
    if (!part.empty())
        parts.push_back(std::move(part));
    if (parts.size() > 1024)
        return std::nullopt;
    std::string const header = "~" + std::to_string(ProtocolVersion) + '~' + token + '~' + std::to_string(sequence);
    std::vector<std::string> messages;
    messages.push_back("BEGIN" + header + '~' + *caps + '~' + std::to_string(rows.size())
        + '~' + std::to_string(parts.size()));
    for (std::size_t index = 0; index < parts.size(); ++index)
        messages.push_back("PART" + header + '~' + std::to_string(index + 1) + '~' + parts[index]);
    messages.push_back("END" + header);
    for (auto const& message : messages)
        if (message.size() > 240)
            return std::nullopt;
    return messages;
}
}

#endif
