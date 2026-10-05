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

Playerbots' AI carries the orders out: walking, fighting, looting, and the
details inside each order.

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
 │  │   LLM's strategies; situation (group/instance)│
 │  ├ CheckStuck: abandon a stuck walk, tell the LLM│
 │  ├ errand: keep walking, use the service on      │
 │  │   arrival, report back                         │
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
  one).
- **`goto <service>`.** Walks to the nearest friendly repair vendor, vendor,
  class trainer, profession trainer, innkeeper, flightmaster, banker or
  auctioneer on this map. The startup index (`AutopilotWorld_Build`) is built
  from `GetAllCreatureData` with npcflags, faction and
  `Trainer::IsTrainerValidForPlayer`. The walk is NewRpg's `go camp`. On
  arrival, the errand does the job:
  - repair vendor: `repair` + `s gray`
  - vendor: `s gray`
  - trainer: `Trainer::TeachSpell` for everything affordable
  - inn: `SetHomebind`
- **`goto zone <name>`.** Walks to the friendly service NPC nearest the
  zone's centre, which is somewhere a walk can end. Same continent only.
- **`quest <id|title>`.** `rpgInfo.ChangeToDoQuest`. The quest must be in the
  log.
- **`rpg <status>`.** NewRpg focus through `AutopilotRpg_Steer`.
- **Anything else** goes to `PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER,
  text, bot)` with the bot as sender, exactly as a master's whisper would.

`Autopilot.DeniedCommands` blocks orders by leading words. Each order's result
("done", "sent", "walking to …", "that quest is not in the log", "denied by
the server", …) is stored and shown in the next prompt, so a failed order
leads to a different one.

## Situations

| Situation | Orders carried out | On entry | On exit |
|---|---|---|---|
| On its own | all | — | — |
| Human's group | `co` only (`WithRealPlayer = 1`); none with `0` | out-of-combat engine back to baseline, errand ends | LLM's strategies replayed, LLM asked |
| Dungeon / battleground | `co` only | same | same |
| Following a bot leader | `co` only | same | same |
| Dead | none | corpse run (see below) | — |

## No teleporting

Playerbots moves random bots by teleport in three places. Autopilot stops
each one for enrolled bots, using public `RandomPlayerbotMgr` calls only:

| Source | Hold |
|---|---|
| Periodic "teleport for level" (`ProcessBot`, every 1–5 h) | `ScheduleTeleport(low, 2h)` every hour |
| Revive after death (`ProcessBot` → `Revive` → `RandomTeleportGrindForLevel`) | Set `dead` and `revive` before `ProcessBot` marks the death; the dead strategy runs the corpse. After `CorpseRunMinutes`, release `revive`. When alive again, clear both |
| NewRpg MoveFarTo stuck for 90 s | At 60 s, `SetMoveFarTo(WorldPosition())` + idle; the LLM is told |

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
