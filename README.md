# mod-questie-bridge

An optional AzerothCore module that lets [Questie-335](https://github.com/Aldori15/Questie)
follow live quest availability, rewards, reset timing, and locations.
Install both the module and addon to use these features.

## Features

- Live holidays, fishing tournaments, Scourge Invasion, and Isle of Quel'Danas unlocks.
- Selected daily/weekly quest pools, ICC weekly quests, and Wintergrasp control.
- Quests explicitly disabled by the running AzerothCore server.
- Opt-in player-specific quest availability conditions for customized servers.
- Global questgiver-template restrictions on zone, continent, and world maps.
- Live NPC questgiver-flag filtering and positive overrides in the player's zone.
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

Options default to `1` (enabled), except experimental `QuestConditions`, which defaults to `0`.
Set an option to `0` to disable it, then run `.reload config`.

| Option | Controls |
| --- | --- |
| `QuestieBridge.Enabled` | Entire bridge |
| `QuestieBridge.Events` | Holidays and event quest unlocks |
| `QuestieBridge.WorldStates` | Requested worldstate values, including the fishing winner |
| `QuestieBridge.Progress` | Scourge activity and World Progress displays |
| `QuestieBridge.Kaluak` | Kalu'ak winner-dependent quests |
| `QuestieBridge.QuestPools` | Daily/weekly quest pool selections |
| `QuestieBridge.QuestDisables` | Quests disabled through the loaded DisableMgr |
| `QuestieBridge.QuestConditions` | Experimental player-specific condition restrictions (opt-in) |
| `QuestieBridge.Questgivers` | Negative live questgiver-flag observations |
| `QuestieBridge.QuestgiverTemplates` | Global template restrictions and required live positive exceptions |
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
| `/qserver` | Connection, versions, live state, and catalog reuse counts |
| `/qserver quest <quest ID>` | Whether a quest is explicitly disabled on the server |
| `/qserver conditions` | Player-specific condition restrictions, when enabled |
| `/qserver giver <NPC ID>` | Global template restriction and unavailable observed spawn IDs |
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
Shared catalogs are cached; player-specific changes send smaller complete updates.
NPC sampling visits the player's visible objects, with positions and visibility checked individually.
Rate changes through `.reload config` and `.reload reputation_reward_rate` need no addon reload
or corrections regeneration. XP bonuses do not increase the maximum-level money bonus.
Reward previews can differ from final awards because of server rounding, caps, or custom rules.

The bridge reports global questgiver-template restrictions across all map zoom levels.
Live NPC spawn observations take precedence locally, including scripted questgiver
grants. Questgiver observations and patrol samples require the creature to be present
at the player's client and pass current distance, phasing, and detection checks.
Distant loaded creatures do not supply live overrides; their template restrictions still apply.

Questgiver restrictions affect NPC queststarter markers and relevant tooltip lines, not quest
eligibility, objectives, turn-ins, or item/gameobject starters. Missing, expired, or
oversized server data restores Questie's normal static behavior.

Patrol markers follow loaded, visible NPCs in your current zone; remote maps keep static locations
and patrol lines. Phase filtering applies within your current subarea. Quest prerequisites,
visibility settings, and manually hidden quests still apply.

Unavailable server information restores Questie's usual behavior. The bridge does not change
quest rewards, requirements, or reset schedules, and players without the addon can play normally.
Entirely new custom quests still need addon data. For a protocol mismatch, install matching builds.
Include `/qserver` output when [reporting an issue](https://github.com/Aldori15/mod-questie-bridge/issues).

## Experimental quest conditions

Reports player-specific restrictions for rewarded quest (8), level (27), and quest state (47)
conditions, including complete AND/OR expressions, safe references, and predicate negation.
Restrictions only hide available starters; accepted quests, objectives, and turn-ins are unaffected.
Unsupported expressions or missing, expired, incomplete, or oversized data keep Questie's normal fallback.

**Disabled by default pending public-server performance validation.** To opt in, set
`QuestieBridge.QuestConditions = 1` and run `.reload config`.

## Protocol

Protocol **21** requires a matching addon. `INFO~1` remains the independent version
diagnostic handshake; a mismatch restores static addon behavior without downgrading.

Snapshots retain `BEGIN` / `PART` / `END`, with compact capabilities in the same field:

```text
WATCH~21~token~worldstateIds~knownCatalogIdOrDash
BEGIN~21~token~sequence~mask~wireRowCount~partCount~catalogId~catalogRowsOrR
PART~21~token~sequence~partIndex~row;row;...
END~21~token~sequence
```

The mask contains one to four uppercase, eight-digit hexadecimal 32-bit words,
lowest capability range first, separated by `.`. Trailing zero words are omitted;
the empty mask is `00000000`. All current capabilities together are `0007FFFF`.
These permanent wire bits are independent of the internal configuration flags:

| Bit | Capability | Bit | Capability |
| --- | --- | --- | --- |
| 0 | EVENTS | 9 | RESETS |
| 1 | VALUES | 10 | PHASES |
| 2 | SCOURGE | 11 | PATROLS |
| 3 | QUELDANAS | 12 | QUESTXP |
| 4 | KALUAK | 13 | QUESTREP |
| 5 | HEARTBEAT | 14 | QUESTMONEY |
| 6 | QUESTPOOLS | 15 | QUESTDISABLES |
| 7 | WINTERGRASP | 16 | QUESTGIVERS |
| 8 | ICC | 17 | QUESTGIVERTEMPLATES |
| | | 18 | QUESTCONDITIONS |

Bits 19–127 are unassigned and rejected when set. Each word is decoded separately to
preserve Lua 5.1 numeric precision.

Messages use addon prefix `QSTSVR` and a maximum 240-byte payload. Snapshot row data is packed
into parts of at most 150 bytes, with at most 4,096 logical rows and 1,024 transmitted parts per snapshot.
Tokens are 1–32 alphanumeric characters; sequences are positive integers up to `9007199254740991`.
The client validates tokens, sequences, counts, and every row before atomic replacement; incomplete
batches are discarded after five seconds. State expires after 30 seconds without a valid snapshot or heartbeat.

Catalog IDs are 64 uppercase hexadecimal SHA-256 digits identifying shared contents and capabilities.
A numeric `catalogRowsOrR` declares the shared prefix of a full batch; `R` reuses a matching fresh catalog.
`wireRowCount` counts transmitted rows; reused rows still count toward the logical limit.
The client publishes only complete validated replacements. Unknown references request a resync.
`WATCH` may advertise a fresh catalog across zone transitions; `-` requests full data, also used if the hint exceeds 240 bytes.

`QUESTCONDITIONS` requires exactly one `P:QC_COUNT:<count>` row plus that many unique
`C:<questId>` rows, limited to 512 quests. These are negative restrictions only; a zero count grants nothing.
Catalog overflow or insufficient snapshot space omits the capability rather than sending a partial catalog.

Heartbeats (`ALIVE~21~token~sequence~snapshotSequence`) renew only an already fresh, complete snapshot.
Patrol batches are sequenced separately; samples expire after five seconds and never renew quest-state freshness.
`INFO~1` diagnoses versions without establishing or renewing availability state.

## License

GPL-2.0-or-later.
