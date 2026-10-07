/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_ICC_H
#define QUESTIE_BRIDGE_ICC_H

#include <cstddef>
#include <string>
#include <vector>

class Player;

constexpr std::size_t QuestieBridgeICCMaxRows = 13;

// The world thread reads copied map-thread observations for this player's current instance only.
void AppendQuestieBridgeICC(Player const* player, std::vector<std::string>& rows);
void RegisterQuestieBridgeICC();

#endif
