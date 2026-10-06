/*
 * Copyright (C) 2026 Questie contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUESTIE_BRIDGE_WINTERGRASP_H
#define QUESTIE_BRIDGE_WINTERGRASP_H

#include <string>
#include <vector>

// Call on the world thread. Reports public state and the stock questgiver's gates.
void AppendQuestieBridgeWintergrasp(std::vector<std::string>& rows);

#endif
