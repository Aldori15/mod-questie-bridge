# mod-questie-bridge

An optional AzerothCore module that sends live game event activity and worldstate
information to [Questie-335](https://github.com/Aldori15/Questie). Questie can use
this information to update available quest icons when events start or stop,
including events started manually with GM commands outside their calendar dates.

**The bridge requires both this server module and the Questie client integration.**

## Features

- Live holiday activity, including manual GM starts and stops.
- Stranglethorn Fishing Extravaganza turn-ins and winner-dependent availability
  for Master Angler and Apprentice Angler.
- Kalu'ak Fishing Derby availability: quest 24803 before Clearwater declares a
  winner, and Better Luck Next Time (24806) afterward.
- Darkmoon Faire activity and questgiver locations in Mulgore, Elwynn Forest,
  and Terokkar Forest, including multiple locations active at the same time.
- Scourge Invasion activity.
- Isle of Quel'Danas quest unlocks, including independent construction projects.
- Authoritative daily and weekly quest pool selections, including custom pools.
  Questie hides inactive choices before players visit the questgiver.

The module reports server state; it does not start events or change quest
requirements. Questie's visibility options, character requirements, and manually
hidden quests still apply. Scourge Invasion and Sun's Reach quests require their
respective visibility options to be enabled in Questie.

## Requirements

- An AzerothCore WotLK server and access to rebuild worldserver.
- World of Warcraft client version 3.3.5a (12340).
- [Questie-335](https://github.com/Aldori15/Questie), with the client bridge changes
  installed on each player who wants this feature.

No SQL installation, Playerbots module, Lua scripting engine, or client executable
patch is required.

## Installation

1. Download this repository into your AzerothCore source directory as
   `modules/mod-questie-bridge`. Alternatively, run this command from the
   AzerothCore source directory:

   ```sh
   git clone https://github.com/Aldori15/mod-questie-bridge.git modules/mod-questie-bridge
   ```

2. Rerun CMake and rebuild source.

3. Copy `QuestieBridge.conf.dist` to `QuestieBridge.conf` in your server's
   `configs/modules` directory.

4. Restart worldserver.

5. Install Questie with the client bridge integration from the
   [Questie repository](https://github.com/Aldori15/Questie). Extract the addon
   into `Interface/AddOns/Questie-335`, then log in or run `/reload`.

6. Run `/qserver` in game. It shows the Questie version, client and server protocol
   versions, module version, and compiled AzerothCore revision. A connected bridge
   reports `Live state` and the
   `EVENTS`, `HEARTBEAT`, `KALUAK`, `QUELDANAS`, `QUESTPOOLS`, `SCOURGE`, and `VALUES` capabilities
   with the default configuration. It also lists every active event ID, including
   events started by GM commands or Lua scripts, followed by fishing and worldstate
   diagnostics. Snapshot and heartbeat counts cover the current addon session.
   Quest pool diagnostics report pool, member, and selected counts, plus the number
   of quests unknown to the installed Questie database. Use `/qserver pool 5678`
   to inspect the raid weekly pool, or substitute another pool ID.

## Configuration

All options default to `1` (enabled). Set an option to `0` to disable it.

```ini
QuestieBridge.Enabled = 1
QuestieBridge.Events = 1
QuestieBridge.WorldStates = 1
QuestieBridge.Progress = 1
QuestieBridge.Kaluak = 1
QuestieBridge.QuestPools = 1
```

- `Enabled`: enables the entire bridge.
- `Events`: reports the game-event catalog, holiday stages, and actual activity.
- `WorldStates`: provides requested persistent worldstate values, including the
  fishing winner flag.
- `Progress`: reports public Scourge Invasion and Quel'Danas progress values.
- `Kaluak`: observes Elder Clearwater's in-memory winner flag without modifying
  his AI, quest rewards, or the event schedule.
- `QuestPools`: reports loaded pool membership and live selected/inactive state.
  No quest ID list, SQL installation, or NPC visit is needed. Existing configuration
  files can add this option; the default is enabled when the option is absent.

Use `.reload config` after changing these options. Disabling `Progress` does not
disable Quel'Danas quest unlock reporting, which uses `Events`.

## Behavior and troubleshooting

Fresh bridge information takes priority over calendar or manual availability
assumptions for supported content. Confirmed inactive events hide that content
even during a calendar window. If the bridge is unavailable, a capability is
disabled, or its information expires, Questie uses its existing calendar and
manual settings. Player preferences are not overwritten.

Event and worldstate changes normally reach Questie within a few seconds. Live
information expires after 30 seconds without a valid update. An unavailable
bridge is checked approximately once per minute; `/reload` starts discovery
again immediately.

The server sends a complete snapshot when Questie connects, subscriptions change,
or reported state changes. While state stays unchanged, it sends a small
heartbeat approximately every 10 seconds. Questie acknowledges its current
snapshot approximately every 20 seconds to renew the server subscription. A
renewal can also receive a heartbeat reply. This reduces repeated addon messages
and client snapshot processing; server state polling continues.

Each heartbeat identifies the complete snapshot it confirms. Questie accepts it
only for a matching, unexpired snapshot. A missed state change or failed renewal
requests a complete replacement; a heartbeat alone cannot restore expired state.
Use `/qserver` to verify that heartbeat counts grow while snapshot counts stay
steady during idle periods. No additional configuration is required.

At each full connection request, the server sends a small diagnostic handshake.
Its format is independent of the state protocol, so incompatible builds can report
their protocol versions without exchanging quest state. `/qserver` distinguishes
a protocol mismatch, a disabled bridge, and no response. There is no protocol
downgrade. Diagnostic replies cannot establish or extend quest-state freshness.

The module version is a release label maintained in the module source. The AC
revision identifies the compiled core revision, not uncommitted module changes.
The reported handshake age is separate from the live-state age and normally grows
while heartbeats keep an unchanged snapshot fresh.

If `/qserver` is not recognized, confirm that your Questie version contains the
client integration and that the addon finished loading. If it reports no fresh
state, check that the module was included in your server build, worldserver was
restarted, and `QuestieBridge.Enabled` is enabled.

Servers without this module retain Questie's existing behavior. Players without
the matching addon integration can continue playing normally.

## Scope

Quest pool selection is read from AzerothCore's loaded PoolMgr state on the world
thread. Membership is discovered from both creature and gameobject quest starters
and verified against PoolMgr. Newly added pool members need no bridge code changes
after the core loads them. Entirely new quests still require Questie database data;
the bridge transports IDs and selection, not quest definitions or locations.

Fresh selections override NPC/comms observations for those pool members. Questie
keeps the observations for fallback and still applies character requirements,
completion, daily limits, holiday gates, and visibility preferences. Accepted
quests and their objectives are retained when the pool rotates. Selection may
include multiple quests or none; it does not assume one selected quest per pool.

Scripted quest variants that indirectly follow a pool, such as Wintergrasp attack
quests, require a separate integration. Quest pool state does not describe
Wintergrasp faction control or every scripted quest-choice mechanism.

Snapshots are bounded to 4096 rows, including event and progress data. If the full
pool catalog cannot fit, the module omits `QUESTPOOLS` rather than advertising a
partial catalog. Questie's existing discovery/comms behavior then applies.

The event transport and persistent worldstate subscriptions are generic. Questie
uses separate client integrations to interpret that data for supported quests.
Custom quests, custom events, and additional scripted availability conditions may
need matching integrations.

## Links

- [Questie-335](https://github.com/Aldori15/Questie)
- [Module repository and issue tracker](https://github.com/Aldori15/mod-questie-bridge)

## License

GPL-2.0-or-later.
