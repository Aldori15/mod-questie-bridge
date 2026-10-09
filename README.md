# mod-questie-bridge

An optional AzerothCore module that lets [Questie-335](https://github.com/Aldori15/Questie)
follow live quest availability, rewards, reset timing, and locations.
Install both the module and addon to use these features.

## Features

- Live holidays, fishing tournaments, Scourge Invasion, and Isle of Quel'Danas unlocks.
- Selected daily/weekly quest pools, ICC weekly quests, and Wintergrasp control.
- Weekly/monthly quest resets and live XP, reputation, and money rates.
- Moving questgiver markers in your current zone, including Orgrim's Hammer and the Skybreaker.
- Phase-dependent locations in supported story areas, including Icecrown, Storm Peaks, and the death knight start.
- World Progress in related Journey quest details and tooltips.

## Installation

Requires an AzerothCore WotLK server you can rebuild and a WoW 3.3.5a (12340) client.

1. Download this repository into `modules/mod-questie-bridge`, or run from your source directory:

   ```sh
   git clone https://github.com/Aldori15/mod-questie-bridge.git modules/mod-questie-bridge
   ```

2. Rerun CMake, rebuild, and install the server.
3. Copy `QuestieBridge.conf.dist` to `QuestieBridge.conf` in the server's `configs/modules` directory.
4. Restart worldserver.
5. Install the matching [Questie-335 addon](https://github.com/Aldori15/Questie), then log in or `/reload`.
6. Run `/qserver`. **Live state** confirms the connection.

No SQL installation is required. Enable **Available Scourge Invasion Quests** or
**Available Sun's Reach Quests** in Questie's icon options to show those quest sets.

## Configuration

All options default to `1` (enabled). Set an option to `0` to disable it, then run `.reload config`.

| Option | Controls |
| --- | --- |
| `QuestieBridge.Enabled` | Entire bridge |
| `QuestieBridge.Events` | Holidays and event quest unlocks |
| `QuestieBridge.WorldStates` | Requested worldstate values, including the fishing winner |
| `QuestieBridge.Progress` | Scourge activity and World Progress displays |
| `QuestieBridge.Kaluak` | Kalu'ak winner-dependent quests |
| `QuestieBridge.QuestPools` | Daily/weekly quest pool selections |
| `QuestieBridge.Wintergrasp` | Wintergrasp quests and questgiver locations |
| `QuestieBridge.ICC` | Weekly quests in the player's current ICC instance |
| `QuestieBridge.Resets` | Weekly/monthly reset timing |
| `QuestieBridge.Phases` | Locations matching the player's story phase |
| `QuestieBridge.Patrols` | Moving questgiver locations in the current zone |
| `QuestieBridge.QuestXP` | Quest XP rates and active quest-XP bonuses |
| `QuestieBridge.QuestReputation` | Quest reputation rates and active reputation bonuses |
| `QuestieBridge.QuestMoney` | Ordinary and maximum-level bonus money rates |

## Checking the connection

| Command | Shows |
| --- | --- |
| `/qserver` | Connection, versions, and live state |
| `/qserver pool <pool ID>` | Selected quests in a pool; for example, `5678` for raid weeklies |
| `/qserver wintergrasp` | Wintergrasp control and battle state |
| `/qserver icc` | Weekly quests in your current ICC instance |
| `/qserver resets` | Next weekly/monthly resets |
| `/qserver phases` | Story-phase location filtering |
| `/qserver patrol [NPC ID]` | Live moving questgiver positions |
| `/qserver xp` | Quest XP rates and active bonuses |
| `/qserver rep [faction ID]` | Reputation modifiers and faction quest rates |
| `/qserver money` | Ordinary and maximum-level bonus money rates |

## Behavior

Changes normally appear within a few seconds. Rehover or reopen reward displays to refresh them.
Rate changes through `.reload config` and `.reload reputation_reward_rate` need no addon reload
or corrections regeneration. XP bonuses do not increase the maximum-level money bonus.
Reward previews can differ from final awards because of server rounding, caps, or custom rules.

Patrol markers follow loaded, visible NPCs in your current zone; remote maps keep static locations
and patrol lines. Phase filtering applies within your current subarea. Quest prerequisites,
visibility settings, and manually hidden quests still apply.

Unavailable server information restores Questie's usual behavior. The bridge does not change
quest rewards, requirements, or reset schedules, and players without the addon can play normally.
Entirely new custom quests still need addon data. For a protocol mismatch, install matching builds.
Include `/qserver` output when [reporting an issue](https://github.com/Aldori15/mod-questie-bridge/issues).

## License

GPL-2.0-or-later.
