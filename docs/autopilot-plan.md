# LLM Autopilot: design

Branch: `feature/llm-autopilot`

## Principle

**The LLM is the bot's master. It makes every behavioural decision; code
never does.**

For each enrolled bot, the LLM decides:

- **Who the character is.** It writes the identity itself.
- **What the character is working toward.** These are measurable goals.
- **What the character does about it.** It gives orders the way a player
  whispers to their own bot: switch strategies, work on this quest, go
  train, repair and sell, spend talents, walk to that zone, queue for a
  dungeon.

Playerbots' AI fights, loots, gathers and casts. Where the bot goes is the
LLM's call, carried out by autopilot's own travel (walking routes, flights,
boats and zeppelins). Playerbots' own overhead controllers (NewRpg and the
like) are off.

Code does two things only:

1. **Reports facts to the LLM.** This covers the quest log, nearby services,
   zones for the level, progress, rewards, deaths, alerts, and what each of
   the LLM's last orders did.
2. **Carries out the orders.** It keeps them standing through playerbots
   resets, stays out of the way where a human or an instance owns the bot, and
   stops playerbots from teleporting the bot.

When the LLM cannot be asked, a bot keeps its last orders. Cost controls
decide *when* the LLM is asked, never *what* a bot does.

Two earlier drafts went the other way, and both were removed:

- The first used preset playstyles, activity weight tables, a dice-roll
  fallback and a boredom formula.
- The second let the LLM toggle only an allow-list of strategies and an rpg
  focus. In practice it behaved like plain NewRpg: the LLM had no visible
  effect.

Do not reintroduce code-side decision logic, and do not shrink the LLM's
reach back to toggles.

This is a module-only feature. Everything below uses public playerbots and
core APIs; nothing requires changing another repository.

## Data flow

```
 world thread (Autopilot_Update, after MapMgr::Update)        worker pool
 ┌──────────────────────────────────────────────────┐
 │ sweep: fixed budget of enrolled bots per tick     │
 │  ├ HoldTeleports: periodic teleport, re-roll,     │
 │  │   revive-teleport (corpse run instead)         │
 │  ├ facts: diary snapshot, zone, gold/skill, goal │
 │  ├ alerts: death streak, gear, bags → ask early  │
 │  ├ Reassert: marker = reset detector; replay the │
 │  │   LLM's strategies; controllers off; situation │
 │  ├ errand: travel legs (walk / fly / boat), then │
 │  │   do the job on arrival, report back           │
 │  └ plan due? → build prompt → Submit ────────────┼─► QueryOllama(Autopilot)
 │ drain decisions ◄────────────────────────────────┼── parse JSON
 │  └ ApplyDecision: identity, goal, RunCommands     │
 └──────────────────────────────────────────────────┘
 PlayerScript hooks (map threads): counters, events, temptations (under g_mutex)
```

## What the LLM decides (one JSON reply)

| Field | Meaning | Applied as |
|---|---|---|
| `identity {style, outlook, profile}` | Who the character is. Required on the first plan; revised only when it really changes | Stored; shown in every later prompt |
| `doing` | Its own label for the current plan | Shown in status and prompts |
| `commands ["nc +grind", "quest 783", "goto trainer"]` | Up to 8 orders, run in order | See *Orders* |
| `minutes` | When to look again (5–180) | `plan_until` |
| `goal {kind, target, text}` | Measurable aim | Resolved against game data; progress measured from the bot |
| `reason` | Why, in its own terms | Diary and later prompts |

## Orders

`mod-ollama-chat_autopilot_commands.cpp`:

- **`nc ...` / `co ...`.** Applied with `ChangeStrategy` directly, not
  through the chat command, because the chat command would also save the
  change into playerbots' own store. Recorded in `Row::strategies` and
  replayed after every playerbots reset (the marker on each engine detects
  one). Turning on `new rpg`, `rpg` or `travel` is refused (see below).
- **`goto <service>`.** Goes to the nearest friendly repair vendor, vendor,
  class trainer, profession trainer, innkeeper, flightmaster, banker or
  auctioneer on this map. The startup index (`AutopilotWorld_Build`) is built
  from `GetAllCreatureData` with npcflags, faction and
  `Trainer::IsTrainerValidForPlayer`. On arrival, the errand does the job:
  - repair vendor: `repair` + `s gray`
  - vendor: `s gray`
  - trainer: `Trainer::TeachSpell` for everything affordable
  - inn: `SetHomebind`
- **`goto zone <name>`.** Goes to the friendly service NPC nearest the zone's
  centre, which is somewhere a walk can end, on any continent.
- **`quest <id|title>`.** The quest must be in the log.
  - **Incomplete:** go to the nearest spawn of a creature it still needs, else
    the centre of the quest's POI marker. On arrival, nothing is done: the
    model turns on what the objective needs.
  - **Complete:** go to the nearest creature that takes it in, then
    playerbots' `talk to quest giver` action, with the bot targeting it.
- **Anything else** goes to `PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER,
  text, bot)` with the bot as sender, exactly as a master's whisper would.

`Autopilot.DeniedCommands` blocks orders by leading words. Each order's result
("done", "sent", "on the way to …", "that quest is not in the log", "denied by
the server", …) is stored and shown in the next prompt, so a failed order
leads to a different one. When a trip ends, the model is asked again after
`QuickReplanSeconds`, because a bot without orders stands still.

## No other controller

Playerbots has its own overhead controllers that pick destinations and
activities: `new rpg`, the older `rpg` wanderer, and the `travel` planner.
Those are the LLM's job here, so they are switched off while the bot is its
own (recorded in the baseline first, so hand-back restores them), and the
model may not turn them on. Fighting, looting, gathering and accepting quests
from NPCs next to the bot are still playerbots' work, driven by the
strategies the model chooses.

## Travel

`mod-ollama-chat_autopilot_travel.cpp` plans a trip as legs. It replans after
every flight or crossing:

| From → to | Legs |
|---|---|
| Another continent | walk to the dock → board → ride → step off; replan |
| Same continent, beyond `Travel.FlightMinYards`, a flight clearly shorter | walk to the nearest flight master → fly to the taxi node nearest the destination; replan |
| Otherwise | walk; then step up to the NPC, if there is one |

**Movement.** Autopilot moves the bot itself (`AutopilotMove_To`): a
`MovePoint` recorded as the bot's last movement at `MOVEMENT_NORMAL`.
Playerbots' own out-of-combat movement waits for it, and combat outranks it.

**Flights.** A flight goes straight through `ActivateTaxiPathTo`, the same
way playerbots' flight action does. TravelMgr's flight-master cache and
`FindTaxiPath` provide the path. If the bot cannot pay, it walks.

**Crossings between continents** are indexed at startup. A breadth-first
search over continents picks the first crossing on a shortest chain the
bot's faction can use. There are three kinds:

- **Boats and zeppelins.** The stop key frames of every non-instance
  `MO_TRANSPORT` in `TransportMgr`. Each dock gets a place to stand ashore
  (the nearest creature spawn) and an owner: NPCs hostile to one side only
  belong to the other. A dock owned only by the other faction is not used.
- **Area-trigger portals** (the Dark Portal both ways), from
  `GetAllAreaTriggerTeleports`. A bot never sends the area-trigger packet, so
  autopilot walks it into the trigger and does what
  `HandleAreaTriggerOpcode` does: `PlayerCannotEnter`, then `TeleportTo`.
- **City portals** (Shattrath, Dalaran, the Silvermoon orb): spellcaster or
  goober gameobjects whose spell teleports to another continent. The bot
  walks up to one and calls `GameObject::Use`. The gameobject's faction
  decides who may use it.

- **Boarding:** once the ship is near its stop and has stopped moving, walk
  straight to a point on deck, found by a dynamic-collision height probe.
  Playerbots' `UpdateAI` makes the bot a passenger.
- **Riding:** hold all movement (`MOVEMENT_FORCED`). The core carries
  passengers across maps.
- **Getting off:** at the far stop, walk straight ashore.
- **Docks and decks:** bots at a dock or aboard are stepped every sweep,
  outside the rotation. A deck point is probed in rings around the ship.
  Failing that, the bot is made a passenger alongside rather than miss the
  crossing. A bot found standing on the ship but not yet attached is attached
  at once.

`MotionTransport::IsMoving` is private, so "docked" means the ship is near
the stop and hasn't moved since the last look.

**Stuck.** If a walk makes no real progress for `Travel.StuckSeconds`, its
route is rebuilt (twice) before the trip fails.

## Long walks

NewRpg's `MoveFarTo` walks a plain pathfinding `MoveTo` only under 70 yards.
Further than that it uses one mmap query (smooth paths cap near 300 yards) or
random forward samples, and after 90 seconds without progress it teleports.
Autopilot doesn't use it. A walk further than 60 yards follows a route
(`mod-ollama-chat_autopilot_route.cpp`) instead. This is mod-city-siege's
two-pass design (`CitySiegePathing.cpp`), built lazily:

1. **Corridor.** `findStraightPath` from the walk cursor to the destination.
   The corners are XY guidance only.
2. **Walk.** Smooth `PathGenerator` legs toward each corner, at most 120
   yards each, halved until one fits. Each aim is re-seated on the ground
   under the walking height. Steep ground and water are costed; lava and
   slime are excluded.
3. **Thin** the dense points to nodes about 28 yards apart, keeping turns.
4. **Hand over** one node at a time with `AutopilotMove_To`.

Building stops once about 12 nodes lie ahead of the bot, and each visit
spends at most `Route.QueriesPerVisit` queries. mmap tiles load with their
grid, so a failure far ahead of the bot is retried once the bot is near.
Without mmaps, the bot walks straight at the destination.

## Situations

| Situation | Orders carried out | On entry | On exit |
|---|---|---|---|
| On its own | all | — | — |
| Human's group | `co` only (`WithRealPlayer = 1`); none with `0` | out-of-combat engine back to baseline, trip ends | LLM's strategies replayed, LLM asked |
| Dungeon / battleground | `co` only | same | same |
| Following a bot leader | `co` only | same | same |
| Dead | none | corpse run (see below) | — |

## No teleporting

Playerbots moves random bots by teleport in two places that matter here.
Autopilot stops both for enrolled bots, using public `RandomPlayerbotMgr`
calls only:

| Source | Hold |
|---|---|
| Periodic "teleport for level" (`ProcessBot`, every 1–5 h) | `ScheduleTeleport(low, 2h)` every hour |
| Revive after death (`ProcessBot` → `Revive` → `RandomTeleportGrindForLevel`) | Set `dead` and `revive` before `ProcessBot` marks the death; the dead strategy runs the corpse. After `CorpseRunMinutes`, release `revive`. When alive again, clear both |

NewRpg's stuck teleport doesn't arise, because NewRpg is off.

`NoRandomize` also holds the periodic re-roll (`SetValue(low, "randomize", 1)`).
That re-roll re-gears the bot and, below level 3 or at the cap, re-levels and
moves it. `AiPlayerbot.AutoTeleportForLevel` is config-only in playerbots, so
autopilot warns at startup if it is on.

## Handing back

Before the LLM's first strategy change, both engines' full strategy lists are
captured (`Row::baseline`, persisted). Handing back means:

1. `ResetStrategies`.
2. For alts, `PlayerbotRepository::Load`.
3. Make each engine equal to the baseline.
4. For alts, save.

Errands end, and held revives are released. Hand-back happens on unenroll,
on `Control = 0`, and when autopilot goes inactive.

## Planning

A plan is asked for in these cases:

- when `plan_until` passes
- when the bot has no identity
- on an event: a level, a goal done or stalled, an errand done or failed, a
  stuck walk, an alert, or leaving a dungeon, battleground, group or flight

Asks are limited by tier (foreground / background / dormant, from real
players' proximity) and a shared token bucket. Planning is refused while the
shared request queue is more than half full, so chat comes first.

The prompt puts static instructions and the command reference first, for
prompt caching, and the character last.

## Persistence

`data/sql/characters/base/2026_10_05_autopilot.sql`:

- **`mod_ollama_chat_autopilot`.** Enrollment, identity, goal, the LLM's
  strategies, last order results, baseline and plan timing. A table from the
  previous draft is migrated in place at startup.
- **`_snapshots`.** Progress samples, thinned with age.
- **`_events`.** Plans, orders, errands, alerts, levels, deaths, quests, loot
  and zones; capped per bot.

Rows for deleted characters are removed by the delete hook. A startup and
hourly anti-join catches deletions made with raw SQL.
