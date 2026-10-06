# LLM Autopilot: design

Branch: `feature/llm-autopilot`

## Principle

**The LLM is the bot's master. It makes every behavioural decision; code
never does.**

For each enrolled bot, the LLM decides:

- **Who the character is.** It writes the identity itself.
- **What the character is working toward.** These are measurable goals.
- **What the character does about it.** It gives orders the way a player
  whispers to their own bot: switch strategies, work on this quest, take
  that quest reward, go train, learn this profession, follow that talent
  spec, sell, buy, repair, walk to that zone, queue for a dungeon.

Playerbots' AI fights, loots, gathers and casts. Where the bot goes is the
LLM's call, carried out by autopilot's own travel (walking routes, flights,
boats, zeppelins and portals). Playerbots' own overhead controllers (NewRpg
and the like) are off.

Code does two things only:

1. **Reports facts to the LLM.** This covers the quest log with reward
   choices, professions and free slots, talent specs and unspent points,
   nearby services, zones for the level, progress, rewards, deaths, alerts,
   queued orders, and what each of the LLM's last orders did.
2. **Carries out the orders.** It keeps them standing through playerbots
   resets, stays out of the way where a human or an instance owns the bot,
   stops playerbots from teleporting the bot, and stops playerbots from
   handing the bot things it did not earn.

When the LLM cannot be asked, a bot keeps its last orders. Cost controls
decide *when* the LLM is asked, never *what* a bot does. Movement pacing
(waiting out a cast, stopping briefly on a long walk) is mechanics, not a
decision.

Two earlier drafts went the other way, and both were removed:

- The first used preset playstyles, activity weight tables, a dice-roll
  fallback and a boredom formula.
- The second let the LLM toggle only an allow-list of strategies and an rpg
  focus. In practice it behaved like plain NewRpg: the LLM had no visible
  effect.

Do not reintroduce code-side decision logic, and do not shrink the LLM's
reach back to toggles. Choices a player makes are the LLM's: which quest
reward, which profession, which talent spec, what to buy and sell.

This is a module-only feature. Everything below uses public playerbots and
core APIs; nothing requires changing another repository.

## Data flow

```
 world thread (Autopilot_Update, after MapMgr::Update)        worker pool
 ┌──────────────────────────────────────────────────┐
 │ answer dungeon-finder proposals                   │
 │ sweep: fixed budget of enrolled bots per tick     │
 │  ├ HoldTeleports: periodic teleport, re-roll,     │
 │  │   revive-teleport (corpse run instead), online │
 │  ├ dead? corpse run, then stop                    │
 │  ├ facts: diary snapshot, zone, gold/skill, goal │
 │  ├ alerts: death streak, gear, bags, idle → ask  │
 │  ├ Reassert: marker = reset detector; replay the │
 │  │   LLM's strategies; controllers off; taxi cheat│
 │  ├ errand: travel legs (walk / fly / boat /       │
 │  │   portal), do the job on arrival, next in queue│
 │  └ plan due? → build prompt → Submit ────────────┼─► QueryOllama(Autopilot)
 │ drain decisions ◄────────────────────────────────┼── parse JSON
 │  └ ApplyDecision: identity, goal, RunCommands     │
 └──────────────────────────────────────────────────┘
 PlayerScript hooks (map threads): counters, events, temptations (under g_mutex)
 ServerScript::OnPacketSent: dungeon-finder proposal ids (any thread)
```

## What the LLM decides (one JSON reply)

| Field | Meaning | Applied as |
|---|---|---|
| `identity {style, outlook, profile}` | Who the character is. Required on the first plan; revised only when it really changes | Stored; shown in every later prompt |
| `doing` | What it is doing now, concretely, in its voice; the orders must carry it out | Shown in status and prompts |
| `commands ["nc +grind", "quest 783", "goto trainer"]` | Up to 8 orders, run in order | See *Orders* |
| `minutes` | When to look again (5–180) | `plan_until` |
| `goal {kind, target, text}` | Measurable aim | Resolved against game data; progress measured from the bot |
| `reason` | Why, in its own terms | Diary and later prompts |

## What the LLM sees

Built in `BuildPromptContext` (`mod-ollama-chat_autopilot.cpp`):

- The character: level, race, class, guild, chat personality, identity,
  current `doing` and goal with measured progress.
- Live strategies, what it is doing right now (walking, flying, waiting for
  a boat), the errand under way and the **orders still queued**.
- **Quest log** with ids and state. A quest ready to turn in that offers a
  choice lists each reward: name, kind (mail chest, one-hand sword, ...),
  armor or damage, stats, and "can't use" when the bot cannot.
- **Professions** with skill levels, and **free primary profession slots**.
- From level 10, **unspent talent points** and the class's **talent specs**
  (playerbots' premade spec names, from its config).
- Nearest services, zones for its level (other continents in brackets),
  gold, durability, free bag slots, concerns (health, gear, bags, money).
- What each of its last orders did, recent decisions, the diary, rewards in
  the last hour, temptations, memories.
- The static command reference (`AutopilotCommands_Reference`), which is the
  LLM's documentation of every order below. Keep it accurate: the model
  believes it.

## Orders

`mod-ollama-chat_autopilot_commands.cpp`. Every order passes
`AutopilotCommands_Normalize` and `AutopilotCommands_IsDenied` first.

- **`nc ...` / `co ...`.** Applied with `ChangeStrategy` directly, not
  through the chat command, because the chat command would also save the
  change into playerbots' own store. Recorded in `Row::strategies` and
  replayed after every playerbots reset (the marker on each engine detects
  one). Turning on `new rpg`, `rpg` or `travel` is refused.
- **`goto <service>`.** Goes to the nearest friendly repair vendor, vendor,
  class trainer, innkeeper, flightmaster, banker or auctioneer on this map.
  The startup index (`AutopilotWorld_Build`) is built from
  `GetAllCreatureData` with npcflags, faction and
  `Trainer::IsTrainerValidForPlayer`. On arrival, the errand does the job:
  - repair vendor: `repair` + `s gray`
  - vendor: `s gray`
  - class trainer: `Trainer::TeachSpell` for everything affordable
  - inn: `SetHomebind`
  - flightmaster: learn the flight point
- **`goto profession <name>`** (or `goto mining`, `goto first aid`): the
  nearest trainer for that profession. A profession trainer's skill is read
  from what it teaches (`TrainerSkill`: a skill or skill-step effect, or a
  learn-spell that leads to one). On arrival everything learnable is taught:
  the profession itself if a slot is free, its next rank, recipes. A bare
  `goto profession` asks which; it never picks the nearest of any kind.
- **`goto zone <name>`.** Goes to the friendly service NPC nearest the zone's
  centre, on any continent. Sub-area names resolve to their zone.
- **`goto hunt`.** The nearest group of monsters of the bot's level.
- **`quest <id|title> [reward <n>]`.** The quest must be in the log.
  - **Incomplete:** go to the nearest spawn of a creature it still needs,
    else the centre of the quest's POI marker. With creatures still to kill,
    the bot then hunts them: the nearest needed one is put in playerbots'
    `prioritized targets` and attacked, and with none in sight it walks to
    the next spawn point. This lasts until the objective is done or
    `QuestHuntMinutes` pass. For items to loot or gather, the model turns on
    what the objective needs.
  - **Complete:** go to the nearest creature that takes it in. The chosen
    reward is taken with `RewardQuest` before playerbots' `talk to quest
    giver` (which would pick by its own stat weights and also takes any
    follow-up quest). Without a choice, the usable reward with the highest
    item level.
- **`abandon <id|title>`.** Drops a quest through `HandleQuestLogRemoveQuest`,
  the client's Abandon button. A quest for another class or race is refused
  by `quest` and marked in the prompt.
- **`talents spec <name>`.** Playerbots' own command: applies one of the
  class's premade specs for the current level. A bare `talents` only prints
  help. `talents autopick` leaves the spec to playerbots.
- **Economy, through playerbots' commands:** `equip upgrade` (best gear in
  the bags, never conjured), `e <item>`, `use <item>`, `open items`, and at
  a vendor `s gray` / `s vendor` / `s <item>`, `b vendor` (buys what the bot
  can use, with its own gold), `repair`.
- **Anything else** goes to `PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER,
  text, bot)` with the bot as sender, exactly as a master's whisper would.
  Commands that need a real master (`craft`, `rpg status`, `trainer`) do
  not work this way.

**Order of orders.** Orders run in the order given. The first `goto` or
`quest` starts a trip; every later order in the plan, except `nc`/`co`,
waits in `Online::errandQueue` and runs when the trip before it ends (so
"goto vendor, b vendor" buys at the vendor). `StepErrand` runs the queue and
asks the model again only when it is empty. A plan with a `goto` or `quest`
replaces whatever is still queued.

`Autopilot.DeniedCommands` blocks orders by leading words; the separator and
`#` prefixes are always refused. Each order's result ("done", "sent", "on
the way to …", "queued (2 in line)", "that quest is not in the log", "denied
by the server", …) is stored and shown in the next prompt, so a failed order
leads to a different one. When a trip ends, the model is asked again after
`QuickReplanSeconds`, because a bot without orders stands still.

## No other controller

Playerbots has its own overhead controllers that pick destinations and
activities: `new rpg`, the older `rpg` wanderer, and the `travel` planner.
Those are the LLM's job here, so they are switched off while the bot is its
own (recorded in the baseline first, so hand-back restores them), and the
model may not turn them on. Fighting, looting, gathering and accepting quests
from NPCs next to the bot are still playerbots' work, driven by the
strategies the model chooses. `nc +maintenance` is playerbots' upkeep with
the bot's own skills and items (learn recipes, disenchant, enchant, destroy
junk, use quest items); the `maintenance` *command* is a handout and refused.

## Earned, not handed out (`NoHandouts`)

Playerbots hands random bots things outside any strategy. For enrolled bots:

| Handout | Where | How autopilot stops it |
|---|---|---|
| Level-up maintenance: talents, trainer and quest spells, skills, consumables, gear upgrade, level-up teleport | `"levelup"` trigger in `"default"` → `auto maintenance on levelup` | Marker strategy multiplier returns 0 |
| Background `ProcessBot(Player*)` (re-roll, refresh, leave group) | `random bot update` pass-through in `"default"` | Multiplier 0 |
| Dungeon-finder accept refreshes a bot queued alone (bags emptied) | `lfg accept` | Multiplier 0; `OnPacketSent` reads the proposal id, `AnswerLfgProposals` accepts it like the client |
| Free full repair on release; spirit healer without sickness | `auto release`, `release`, `spirit healer` | Multiplier 0 when alone in the open world; autopilot releases through `HandleRepopRequestOpcode` |
| Five-death re-roll revive | `find corpse` with "death count" ≥ 5 | "death count" zeroed each visit |
| Taxi cheat (fly anywhere) | `PlayerbotAI` constructor | `SetTaxiCheater(false)` in `Reassert` |
| `maintenance`, `autogear`, `bis` orders (free gear and supplies) | chat commands | Refused in `AutopilotCommands_IsDenied` |
| Periodic re-roll and refresh (bags emptied, money set) | `ProcessBot` timers | `"randomize"` held, `ScheduleTeleport`; also at `OnStartup` |

A zero multiplier makes `Engine::DoNextAction` drop the action. It does not
stop `ExecuteAction` / `DoSpecificAction`, so never call those for these
actions. The marker is on all three engines (dead included) for this.

Playerbots' level brackets (`RandomBotLevelMgr`, `AiPlayerbot.LevelBrackets.*`)
skip friend-listed bots (`IgnoreFriendListed`, default on), so enrolled bots get a `character_social` row owned by
guid 0 (`SyncBracketShield`, rebuilt at startup, note `ollama autopilot`).

Not reachable per bot: playerbots' global cheats (`AiPlayerbot.BotCheats`,
e.g. `food`), and playerbots' level reset at the cap
(`AiPlayerbot.ResetBotLevel.*`, off by default), which ignores friend lists;
its `ExcludeNames` is the operator's.

## Travel

`mod-ollama-chat_autopilot_travel.cpp` plans a trip as legs. It replans after
every flight or crossing:

| From → to | Legs |
|---|---|
| Another continent | walk to the dock → board → ride → step off; or walk into the Dark Portal; or use a city portal; replan |
| Same continent, beyond `Travel.FlightMinYards`, a flight clearly shorter | walk to the nearest known flight master → fly to the known taxi node nearest the destination; replan |
| Otherwise | walk; then step up to the NPC, if there is one |

**Movement.** Autopilot moves the bot itself (`AutopilotMove_To`): it
computes a navmesh path on the core's bot filter and walks it with
`MoveSplinePath` (an escort spline), recorded as the bot's last movement at
`MOVEMENT_NORMAL`. Playerbots' own out-of-combat movement waits for it.
- No path, no move: never a straight line through walls or up cliffs. The
  only straight step is at most 8 yd, in sight and without a climb.
- A slope-checked path that gets nowhere (a river bank counts as too steep a
  step) is retried without the slope check, still on the bot filter.
- A two-point path is padded to three, because the escort generator re-paths
  two points without our filter.
- **Combat:** an attacked bot's walk is stopped at once (`AutopilotMove_Yield`;
  a caster cannot cast through an escort spline), and resumes 4 s after the
  fight.
- **Casts:** the walk waits while the bot casts, and 2 s after. Every 30 s of
  walking it stops for 2 s, because playerbots never starts a cast-time spell
  (a pet summon, a buff) while the bot moves.

**Flights.** Only between flight points the bot has discovered
(`m_taxi.IsTaximaskNodeKnown`), as for a player. A bot passing within 30 yd
of a flight master learns its flight point. A flight goes through
`ActivateTaxiPathTo` with a path from TravelMgr's flight-master cache and
`FindTaxiPath`. If the bot cannot pay, it walks.

**Crossings between continents** are indexed at startup. A breadth-first
search over continents picks the first crossing on a shortest chain the
bot's faction can use:

- **Boats and zeppelins.** The stop key frames of every non-instance
  `MO_TRANSPORT` in `TransportMgr`. Each dock gets a place to stand ashore
  and an owner: NPCs hostile to one side only belong to the other. A dock
  owned only by the other faction is not used.
- **Area-trigger portals** (the Dark Portal both ways), from
  `GetAllAreaTriggerTeleports`. A bot never sends the area-trigger packet, so
  autopilot walks it in and does what `HandleAreaTriggerOpcode` does.
- **City portals** (Shattrath, Dalaran, the Silvermoon orb): spellcaster or
  goober gameobjects whose spell teleports to another continent; the bot
  uses them with `GameObject::Use`.

Boarding:
- Wait ashore until the ship is near its stop and has not moved since the
  last look (`MotionTransport::IsMoving` is private).
- Walk to the pier first, then probe for a deck point (dynamic-collision ray
  cast with headroom). Never board blind: with no deck found, wait for the
  next docking. A bot standing on the deck is attached at once.
- **Riding:** hold all movement (`MOVEMENT_FORCED`); the core carries
  passengers across maps. **Getting off:** walk straight ashore.
- Bots at a dock or aboard are stepped every sweep, outside the rotation.

**Staying on the road.** While a trip is under way (the travelling set,
`AutopilotStrategy_SetTravelling`), the marker multiplier zeroes grind's
`attack anything` and `move random`: no pulling neutral beasts, no
wandering. The combat engine still answers attackers.

**Another way.** A crossing whose leg fails while the bot is still on its
side is added to the trip's `avoid` list and the trip replans, so a portal
that does nothing falls back to a boat.

**Stuck.** If a walk makes no real progress for `Travel.StuckSeconds`, its
route is rebuilt (twice) before the trip fails and the model is told.

## Long walks

A walk further than 60 yards follows a route
(`mod-ollama-chat_autopilot_route.cpp`), mod-city-siege's two-pass design,
built lazily:

1. **Road anchors.** Playerbots' travel-node road network:
   `TravelNodeMap::getRoute(node, node)` between the nodes nearest the start
   and the destination, under our own `try_to_lock` on its mutex, walk links
   only. Never `getFullPath` (it leaks its lock on an empty route). An anchor
   a corridor cannot get closer to is skipped.
2. **Corridor.** `findStraightPath` toward the next anchor or the
   destination, at most 250 yards ahead, halved while the far tile is not
   loaded. Corners are XY guidance only. The aim point is snapped onto
   walkable navmesh first (`SnapToMesh`): from inside a building a point
   straight ahead is in its walls, and the core answers that exactly like an
   unloaded tile. `TileLoaded` tells the two apart for the failure message.
3. **Walk.** Smooth `PathGenerator` legs toward each corner, at most 120
   yards each, halved until one fits, re-seated on the ground. A bot on a
   slope the filter excludes steps off onto walkable ground first.
4. **Thin** the dense points to nodes about 28 yards apart, keeping turns,
   and **hand over** one node at a time.

Each visit spends at most `Route.QueriesPerVisit` queries. mmap tiles load
with their grid, so a failure far ahead of the bot is retried once the bot
is near. Without mmaps, the bot walks straight at the destination.

## Situations

| Situation | Orders carried out | On entry | On exit |
|---|---|---|---|
| On its own | all | — | — |
| Human's group | `co` only (`WithRealPlayer = 1`); none with `0` | out-of-combat engine back to baseline, trip and queue end | LLM's strategies replayed, LLM asked |
| Dungeon / battleground | `co` only | same | same |
| Following a bot leader | `co` only | same | same |
| Dead | none | corpse run (see below) | — |

## No teleporting, and death

Playerbots moves random bots by teleport. Autopilot holds this for enrolled
bots with public `RandomPlayerbotMgr` calls only, from `OnStartup` (before
bots log in) and every hour after:

| Source | Hold |
|---|---|
| Periodic "teleport for level" (`ProcessBot`, every 1–5 h) | `ScheduleTeleport(low, 2h)` |
| Periodic re-roll (`NoRandomize`) | `SetValue(low, "randomize", 1)` |
| Revive after death (`ProcessBot` → `Revive`) | Set `dead` and `revive` before `ProcessBot` marks the death; clear both when alive again |
| Logout when online time lapses; random pick at start (`KeepOnline`) | `SetValue(low, "add", 1)`, random-bot accounts only |

**Corpse run** (alone, open world): release like the client
(`HandleRepopRequestOpcode`), walk the ghost to the body along a route (no
flights), reclaim within 35 yd through `HandleReclaimCorpseOpcode` once the
core's reclaim delay (30/60/120 s) is up. A failed trip is retried after
30 s. After `CorpseRunMinutes`, the spirit healer's resurrection
(`SendSpiritResurrect`: sickness, durability loss), never playerbots'
re-roll revive.

## Handing back

Before the LLM's first strategy change, both engines' full strategy lists are
captured (`Row::baseline`, persisted). Handing back means:

1. `ResetStrategies`.
2. For alts, `PlayerbotRepository::Load`.
3. Make each engine equal to the baseline.
4. For alts, save.

Errands and the queue end, and held revives are released. Hand-back happens
on unenroll, on `Control = 0`, and when autopilot goes inactive.

## Planning

A plan is asked for in these cases:

- when `plan_until` passes
- when the bot has no identity
- on an event: a level, a goal done or stalled, an errand (and its queue)
  done or failed, a stuck walk, an alert, idling, or leaving a dungeon,
  battleground, group or flight

Asks are limited by tier (foreground / background / dormant, from real
players' proximity) and a shared token bucket. Planning is refused while the
shared request queue is more than half full, so chat comes first.

The prompt puts static instructions and the command reference first, for
prompt caching, and the character last. The last prompt and raw reply per
bot are kept for the monitor addon (`AutopilotPlanner_LastExchange`).

## Persistence

`data/sql/characters/base/2026_10_05_autopilot.sql`, repaired at startup by
`mod-ollama-chat_autopilot_schema.cpp`:

- **`mod_ollama_chat_autopilot`.** Enrollment, identity, goal, the LLM's
  strategies, last order results, baseline and plan timing.
- **`_snapshots`.** Progress samples, thinned with age.
- **`_events`.** Plans, orders, errands, alerts, levels, deaths, quests, loot
  and zones; capped per bot.

Rows for deleted characters are removed by the delete hook. A startup and
hourly anti-join catches deletions made with raw SQL.
