# mod-questie-bridge

An optional AzerothCore module that lets [Questie-335](https://github.com/Aldori15/Questie)
follow live server events and world progress, including holidays activated outside their calendar dates.
Install both this server module and Questie-335 to use the bridge.

## Features

- Holidays started or stopped by GM commands or server scripts, including simultaneous Darkmoon Faire locations.
- Zalazane's Fall quests appear while the server event is active.
- Stranglethorn Fishing Extravaganza and Kalu'ak Fishing Derby quests before and after a winner is declared.
- Scourge Invasion activity and Isle of Quel'Danas quest unlocks.
- The server's selected daily and weekly pool quests, before visiting the questgiver. Custom pools are supported.
- ICC weekly quests follow the selected family, raid size, and unlocks in each player's current raid instance.
- Wintergrasp quests and questgiver locations as faction control changes.
- World Progress in related Journey quest details and hover tooltips: Sun's Reach phase and construction
  percentages, plus Scourge Invasion victories and remaining necropolises.

Questie's visibility options, character requirements, and manually hidden quests still apply.
Accepted quests remain tracked when events, pool selections, or faction control change.
The module reports server state without changing event schedules or quest requirements.

## Installation

Requires an AzerothCore WotLK server that you can rebuild, a WoW 3.3.5a (12340) client,
and [Questie-335](https://github.com/Aldori15/Questie). No SQL installation or Lua scripting engine is required.

1. Download this repository into your AzerothCore source directory as `modules/mod-questie-bridge`,
   or run this command from the source directory:

   ```sh
   git clone https://github.com/Aldori15/mod-questie-bridge.git modules/mod-questie-bridge
   ```

2. Rerun CMake, rebuild, and install your updated server binaries and configuration files.
3. In the server's `configs/modules` directory, copy `QuestieBridge.conf.dist` to `QuestieBridge.conf`.
   The default settings enable all bridge features.
4. Restart worldserver.
5. Install [Questie-335](https://github.com/Aldori15/Questie) in `Interface/AddOns/Questie-335`,
   then log in or run `/reload`.
6. Run `/qserver` in game. **Live state** confirms the connection.

To display Scourge Invasion or Sun's Reach quests, enable **Available Scourge Invasion Quests**
or **Available Sun's Reach Quests** in Questie's icon options.

## Configuration

All options default to `1` (enabled). Set an option to `0` to disable it,
then run `.reload config` in game.

| Option | Controls |
| --- | --- |
| `QuestieBridge.Enabled` | The entire bridge |
| `QuestieBridge.Events` | Holiday activity and event-based quest unlocks |
| `QuestieBridge.WorldStates` | Requested worldstate values, including the Stranglethorn winner flag |
| `QuestieBridge.Progress` | Scourge Invasion activity and Journey world progress |
| `QuestieBridge.Kaluak` | Kalu'ak winner-dependent quest availability |
| `QuestieBridge.QuestPools` | Daily and weekly quest pool selections |
| `QuestieBridge.Wintergrasp` | Wintergrasp state, quest availability, and questgiver location filtering |
| `QuestieBridge.ICC` | Weekly quest selection and unlocks in the player's current ICC raid instance |

Disabling `Progress` also removes live Scourge activity reporting. Quel'Danas quest unlocks use `Events`
and continue working independently of progress displays.

## Checking the connection

- `/qserver`: connection status, versions, active events, and reported server state.
- `/qserver pool <pool ID>`: selected and inactive quests in a pool. For example, `/qserver pool 5678`
  shows the raid weekly pool.
- `/qserver wintergrasp`: faction control, battle activity, and quest availability rules.
- `/qserver icc`: the current ICC instance, raid difficulty, weekly quest family, and availability gates.

ICC selection applies while the player is inside that raid. Outside ICC, Questie keeps its existing behavior.

Changes normally appear within a few seconds. Small heartbeats keep unchanged information current
without repeatedly sending the full state. Heartbeat counts increasing while snapshots stay steady is normal.

If `/qserver` is unavailable, check that Questie-335 is installed and loaded. If it reports no connection,
check that the module was included in your build, worldserver was restarted, and `QuestieBridge.Enabled = 1`.
For a protocol mismatch, install matching Questie and module builds. Include `/qserver` output when reporting an issue.

## Optional behavior

Live server information takes priority for supported content. If the module is absent, a feature is disabled,
or updates stop for 30 seconds, Questie returns to its existing calendar detection, manual settings,
and daily quest discovery. Players without the addon can continue playing normally.

The bridge supplies state for quests already known to Questie. Entirely new custom quests still need addon
data for their names and locations; custom scripted quest rules may need additional integration.
Kalu'ak winner state is available while Elder Clearwater's supported NPC script is loaded.

## Links

- [Questie-335](https://github.com/Aldori15/Questie)
- [Report a module issue](https://github.com/Aldori15/mod-questie-bridge/issues)

## License

GPL-2.0-or-later.
