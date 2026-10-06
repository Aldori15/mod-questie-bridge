/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QUESTIE_BRIDGE_KALUAK_H
#define QUESTIE_BRIDGE_KALUAK_H

#include <string>

// "0" = no winner, "1" = winner declared, "?" = NPC AI unavailable/ambiguous.
std::string GetQuestieBridgeKaluakFinished();
void RegisterQuestieBridgeKaluak();

#endif
