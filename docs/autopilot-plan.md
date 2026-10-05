# LLM Autopilot — design plan

Branch: `feature/llm-autopilot`

## What this is

A playerbots strategy, `autopilot`, that hands a bot's **macro** decisions to
the LLM. The LLM never steers, targets or casts. It decides *what kind of
session this character is having*: questing, exploring, grinding, levelling a
profession, running dungeons, resting in town. It does this by switching
playerbots strategies and NewRpg statuses that already exist. Playerbots keeps
doing all the moment-to-moment work, including combat.

Each autopilot bot has one **playstyle**: explorer, speed-leveller, crafter,
roleplayer, casual and so on. It also has an **awareness** level, from fully
in-character to a player who knows it's a game. The bot keeps a long-term
**goal**, a **boredom** level, and a **history** of snapshots and events in the
database, so its decisions build on what it has actually done.

It is built from the `mod-ollama-bot-buddy` experiment, but deliberately avoids
that module's mistakes:

| bot-buddy did | autopilot does |
|---|---|
| `ClearStrategies` every tick and micro-drove the bot (coords, GUIDs, spells) | leaves playerbots' engines running and picks *strategies and goals* |
| ran LLM output on a detached thread (`MotionMaster`, `Attack`, opcode handlers) | worker does HTTP + JSON parsing only; all changes are applied in `OllamaDispatch_Update` on the world thread |
| one bot, hard-coded by name | enrollment by strategy, command or configured share of random bots |
| a request as soon as the last one returned, no timeout | a decision interval per bot, a global concurrency cap, and the existing HTTP timeouts |
| in-memory history of the last 5 commands | snapshots, events and goals in the DB |
| a ~200-line all-caps rules prompt aimed at a 1B model | a compact prompt plus a fixed JSON schema; the menu of choices comes from config |

## Hard constraints (from CLAUDE.md, and verified against this fork)

- **World thread only** for anything that touches `Player` or `PlayerbotAI`:
  building the snapshot and prompt, applying strategies, recording events.
  Workers get a finished prompt string and return a parsed decision.
- **Workers never read config strings.** The activity menu and persona text are
  formatted into the prompt at submit time. The model override goes into
  `OllamaEndpointSettings` through the snapshot.
- **Playerbots has no extension hooks.** However, every bot's
  `AiObjectContext` is one of the ten class subclasses. Each subclass has
  **public static** `sharedStrategyContexts` / `sharedActionContexts`
  (`PriestAiObjectContext.h:25`, etc.). Each per-bot list holds a *reference*
  to the shared creator map (`NamedObjectContext.h:162`). So calling
  `Add(new AutopilotStrategyContext())` on all ten lists after
  `AiObjectContext::BuildAllSharedContexts()` registers the strategy for every
  bot, with no edit to mod-playerbots. We do this in our
  `WorldScript::OnStartup`. Playerbots builds its contexts in
  `OnBeforeWorldInitialized`, and bots log in later.
- **Calling `ChangeStrategy` directly does not persist**, and random bots get
  `ResetStrategies()` often: on regroup, BG/LFG, resurrect `Refresh`, and
  `Randomize`. So our own DB table is the source of truth for enrollment and
  for the current activity. The planner re-asserts both when it sees that a
  reset wiped them.
- No AH, bank or sell *strategies* exist, only chat actions (`sell`, `repair`,
  `bank`, `trainer`, `maintenance`, `autogear`). Auction posting has no
  playerbots action at all (see Phase 5).

## Architecture

```
            world thread                              worker pool (existing)
 ┌───────────────────────────────────────┐
 │ Autopilot_Update(diff)  [throttled]    │
 │  ├─ Recorder: snapshot due? → DB       │
 │  ├─ Guards: durability/bags/deaths     │ (deterministic, no LLM)
 │  ├─ re-assert strategies after reset   │
 │  └─ pick bots due for a decision ──────┼──► Task{Plan, prompt, botGuid}
 │                                        │        QueryOllama(kind=Autopilot)
 │ OllamaDispatch_Update                  │        parse + schema-validate JSON
 │  └─ PlanCompletion → Autopilot_Apply ◄─┼─────── Completion{AutopilotDecision}
 │       re-resolve bot by guid           │
 │       whitelist-check activity         │
 │       ChangeStrategy / rpgInfo / do X  │
 │       record event + goal in DB        │
 └───────────────────────────────────────┘
 Event hooks (events.cpp: kill/loot/death/quest/level/achievement/learn)
   └─ Recorder_Event(...)  → in-memory ring + async DB insert
```

New files (they are globbed, so no CMake change):

- `mod-ollama-chat_autopilot.{h,cpp}`: enrollment, scheduler, prompt builder,
  decision applier, guards.
- `mod-ollama-chat_autopilot_strategy.{h,cpp}`: the `autopilot` marker strategy
  and its registration into the ten class contexts. This is the only file that
  includes playerbots engine headers.
- `mod-ollama-chat_progress.{h,cpp}`: snapshot recorder and event log
  (DB-backed, bounded), progress deltas, and boredom/satisfaction metrics.
- `mod-ollama-chat_playstyle.{h,cpp}`: playstyle and awareness profiles,
  assignment, and prompt fragments.

## Selection: which bots the server owner puts under autopilot

There are two separate questions, and they are kept apart on purpose:

- **Enrolled**: this bot *is* an autopilot character. It has a playstyle, a
  goal and a diary. Enrollment is persistent and cheap: a DB row plus a small
  in-memory struct.
- **Planning tier**: how much *thinking* this bot gets right now. Tiers are
  re-evaluated every sweep, and they are what actually costs anything (see
  Scaling).

### Enrollment sources

These can all be combined. A bot is enrolled if any *include* source matches
and no *exclude* rule does.

| source | setting / command | notes |
|---|---|---|
| explicit, per bot | `.ollama autopilot on`/`off <bot>` | always wins over rules, both ways (`off` writes an opt-out row) |
| master opt-in | `nc +autopilot` from the bot's master | only when `Autopilot.AllowMasterEnroll = 1` |
| random bots | `Autopilot.Select.RandomBotPercent` | stable: chosen by guid hash, so the same bots stay in as the percent changes |
| alt / addclass bots | `Autopilot.Select.AltBots` | default `0`, because these already have a human master |
| guilds | `Autopilot.Select.Guilds = "id,id"` | e.g. an all-bot RP guild |
| accounts | `Autopilot.Select.Accounts = "id,id"` | lets the operator dedicate bot accounts |
| names | `Autopilot.Select.Include = "Name,Name"` / `.Exclude` | small hand-picked casts |
| level band | `Autopilot.Select.MinLevel` / `.MaxLevel` | filters the rule sources only, not explicit `on` |
| hard cap | `Autopilot.MaxEnrolled` | stable cap: the lowest guid hash wins, so the set doesn't churn |

Rules are evaluated **once per bot at login**, and again on `.ollama reload`,
never per tick. The result is cached in the bot's in-memory struct. Explicit
`on`/`off` rows are loaded with the rest of the autopilot table at startup.

The `autopilot` strategy is a visible marker that mirrors enrollment. It is
not the switch, because playerbots resets wipe it constantly; the sweep adds
it back wherever it is missing. `nc -autopilot` therefore can't unenroll a
bot, since it looks the same as a reset. The conf file says so.

Playstyle distribution can be shaped per source. For example,
`Autopilot.Select.Guild.<id>.Playstyle = roleplayer` pins a guild's
playstyle. Otherwise `Autopilot.Playstyle.Weights` applies.

**`.ollama autopilot preview`** dry-runs the current rules against online bots
and prints:

- matched counts per source
- the tier split
- the estimated LLM calls per hour at current settings

This lets the operator size the feature before turning it on.

### Control in every situation

Autopilot controls four layers. Only the activity layer ever stands down, and
only while something else already owns the bot's movement. An LLM round trip
takes seconds, so nothing is decided *in the moment*. Combat, death and
dungeon behaviour are **decided ahead of time** and applied instantly by C++
when the situation starts.

| layer | what it is | decided by | applied |
|---|---|---|---|
| **disposition** | standing behaviour modifiers that hold in every state, combat included | LLM, at goal time (from playstyle + personality) | always; reapplied after resets |
| **playbook** | which preset to use in each situation: dungeon, battleground, grouped with a player, dead, low health | LLM, at goal time, as part of the same answer | instantly by C++ when the situation starts |
| **boundary decisions** | "what now?" at the edges: dungeon finished, BG over, left a group, died repeatedly, landed from a flight | LLM, event-triggered and high in the budget queue; policy fallback | when the answer arrives |
| **activity** | where to go and what to work on | policy / LLM | suspended only while movement is owned elsewhere (see below) |

**Dispositions** are presets in conf. The model picks names, not strategies,
just as it does for activities. They only ever use a whitelist of *modifier*
strategies that exist in this fork (`StrategyContext.h`). Class, spec and role
strategies (tank/heal/dps, `ranged`/`close`) are **never** touched. Examples:

```
OllamaChat.Autopilot.Disposition.cautious = "co:+flee,+potions,+avoid aoe,+threat,-aggressive"
OllamaChat.Autopilot.Disposition.bold     = "co:+aggressive,-flee,-threat"
OllamaChat.Autopilot.Disposition.greedy   = "nc:+loot,+gather;co:+attack tagged"
OllamaChat.Autopilot.Disposition.frugal   = "co:+save mana,-potions"
OllamaChat.Autopilot.Disposition.social   = "nc:+emote,+chat,+start duel"
OllamaChat.Autopilot.Disposition.reserved = "nc:-emote,-start duel"
```

A bot holds one disposition per axis (risk, greed, sociability), so a
"cautious greedy reserved" crafter really does play differently in a fight
from a "bold social" speedrunner. Risk tolerance is where self-preservation
reaches into combat.

**Playbook** is a small object in the goal-time answer:

```json
"playbook": { "dungeon": "cautious", "battleground": "bold",
              "with_player": "follow_lead", "on_death": "wait_for_res",
              "low_health": "flee" }
```

Every value must be a known preset name for that situation, so it is validated
like everything else. On death, `wait_for_res` vs `release` controls how long
the `dead` engine waits before releasing. Repeated deaths are a boundary
decision.

#### Where the activity layer stands down, and what still happens

| situation | activity layer | still active |
|---|---|---|
| **grouped with a real player** | depends on `Autopilot.WithRealPlayer` (below) | disposition, playbook; the bot can *voice* its wants ("could we stop at the forge?") through normal chat |
| **dungeon / raid** | off: the dungeon strategies and the group own movement | disposition (via the dungeon playbook entry); on finish, a boundary decision: run again, go sell, back to the goal |
| **battleground / arena** | off: the BG strategies own movement | disposition (BG playbook entry); on end, a boundary decision |
| **combat** | off for the length of the fight | disposition, low-health playbook; nothing waits on the LLM |
| **dead** | off | `on_death` playbook; a death streak triggers a boundary decision |
| **flight path** | off | on landing, a boundary decision if the flight was the activity's goal |

`Autopilot.WithRealPlayer` is for the server owner, because how much a bot
should assert itself in a human's group is a taste call:

- `0`: hands off. Not even dispositions are changed.
- `1` (**default**): dispositions and playbook only. The player leads.
- `2`: as `1`, plus the bot voices its goals and wants in party chat, and
  periodically asks to do something its playstyle wants.
- `3`: full autonomy. The bot may leave the group when its goals diverge, and
  stays polite about it.

**A human's choice always wins.** When a bot is grouped with a real player,
the applier compares the bot's live strategies with what autopilot last
applied. Any managed strategy that differs was changed by the player (`co -flee`,
for example). That strategy is then **locked** against autopilot until the
group breaks up, so autopilot never fights a human over a toggle.

The global `Autopilot.Enable = 0` is the only thing that switches all four
layers off. Nothing is torn down, so turning it back on resumes where the bot
left off.

## Scaling: making hundreds of bots affordable

The expensive part is LLM calls. World-thread work is cheap as long as it is
spread out. The design keeps **LLM cost bounded by a budget, not by bot
count**: adding bots means the average bot thinks less often, not that the
server does more.

### 1. Split the thinking: LLM sets goals, C++ picks activities

The LLM only gets the *strategic* question: "what is this character working
toward, and why?" It is asked when a goal completes or fails, when boredom is
high, or about every `GoalRefreshMinutes` (default 90).

The *tactical* question, "which activity serves that goal right now?", is
answered by a deterministic **policy** in C++. It runs every few minutes and
costs almost nothing. Each goal kind has a set of candidate activities:

- `reach_skill mining 150` → `gather`, then `town` to train
- `explore_zone Feralas` → `travel`, then `explore`
- `reach_level 30` → `quest` / `grind` / `dungeon`

The policy scores those candidates by playstyle weights, boredom, recent
reward rates and the guards.

So a bot costs roughly **one LLM call an hour instead of four or more**, and it
still acts on its goal in between.

### 2. Tiers

| tier | who | what it gets |
|---|---|---|
| **foreground** | a real player is within `Autopilot.ForegroundRange` (default: same zone), or the bot's guild has a real player online | LLM goals, plus LLM activity choice every `DecisionIntervalMinutes`; may `say` its reasoning |
| **background** | enrolled, nobody watching | LLM goals only; activities from the policy; no `say` |
| **dormant** | no real player on the continent for `Autopilot.DormantAfterMinutes`, or the budget is exhausted | policy only. The current goal is kept, or one is drawn from a playstyle template list in conf. Zero LLM calls |

Tiers are computed from the existing `OllamaWorldSnapshot`, in one pass over
real players per sweep rather than one per bot. This is the same reasoning the
chat module already uses: thinking nobody sees is the first thing to cut.

### 3. One global budget

- `Autopilot.LlmCallsPerHour` is a token bucket shared by every bot.
- Calls waiting for budget sit in a priority queue, in this order:
  1. foreground before background
  2. goal completed or failed
  3. boredom over the threshold
  4. periodic refresh
- When the bucket is empty, bots carry on under the policy until there is room
  again.
- `Autopilot.MaxConcurrentPlans` caps how many requests are in flight.
- Planning pauses whenever the shared dispatch queue is more than half full,
  so chat replies always come first.
- `Autopilot.Model` can point planning at a separate, cheaper model.

### 4. Make each call cheap

- **Prompt order for caching.** The prompt goes static-first, dynamic-last:
  instructions, schema and activity menu, then the playstyle/awareness block,
  then bot state and history. Ollama reuses the KV cache for an identical
  prefix, and OpenAI and Anthropic prompt caching key on the prefix too, so
  most of each prompt costs nothing to re-read.
- **Compact state.** Only foreground bots get the expensive visible-objects
  scan in `GenerateBotGameStateSnapshot`. Background goal-setting gets macro
  state and progress deltas, which is what a strategic decision needs anyway.
- **Optional batching (later phase).** With `Autopilot.BatchSize` > 1, one
  request plans several background bots and returns a JSON array. Bots are
  grouped by playstyle so the shared prefix stays large. This is off by
  default because small models lose track.

### 5. Keep the world thread flat

- **Throttled, round-robin sweep.** `Autopilot_Update` runs every
  `SweepIntervalMs` (default 1000). Each run advances a cursor over the
  enrolled bots that are online and handles at most `Autopilot.BotsPerSweep`
  of them. The cost per tick stays constant however many bots are enrolled.
- **Staggered snapshots.** Each bot's snapshot is due at
  `(guid hash mod interval)`. At a 30-minute interval, 1000 bots means about
  one snapshot every 2 s, not 1000 at once. A recorder snapshot reads only
  `Player` fields (level, money, durability, bags, skills) and does no grid
  scans.
- **Events are counters.** The `events.cpp` hooks bump counters in the bot's
  struct, which is O(1). Only notable events (level, death, rare loot, quest,
  goal) become DB rows.
- **Batched DB writes.** Snapshot and event rows queue in memory and are
  flushed in one async transaction every `FlushIntervalSeconds`.
- **Downsampled history** instead of plain trimming. A trim pass at flush time
  keeps:
  - every snapshot for the last 24 hours
  - one per hour for the week before that
  - one per day after that

  A long-lived bot's history stays at a few hundred rows.
- **Lazy loading.** Per-bot history is read from the DB only when a prompt
  needs it, and cached until the next snapshot. Nothing is read at login.

### Rough numbers to validate in Phase 2

Take 500 enrolled bots with 1–2 real players online:

- ~20 foreground bots at ~4 calls/h ≈ 80/h
- ~480 background/dormant bots at ≤ 1/h, capped by the budget
- the default `LlmCallsPerHour = 300` works out to about 5 calls a minute

The world-thread sweep handles `BotsPerSweep = 25` bots per second, so it
cycles all 500 every 20 s. Measure the sweep with the existing
`.ollama status` timing before raising any defaults.

Prerequisite: `OllamaChat.EnableChatBotSnapshotTemplate = 1`. If autopilot is
enabled without it, startup logs a warning and autopilot stays off.

## Playstyles and awareness

Each enrolled bot gets one **playstyle** (its main way of playing) and one
**awareness** level. Both are stored in the DB and editable by command.

| playstyle | favours | rewarded by | boredom |
|---|---|---|---|
| explorer | wandering, travel, new zones, flight paths | discovery, scenery | fast in visited zones |
| speedrunner | efficient quests, chained turn-ins, dungeons at level | xp/hour | rises when xp/hour drops |
| quester | finishing quest chains, story | quests completed | slow |
| grinder | killing, farming | xp, loot | slow |
| crafter / merchant | gathering, profession skill-ups, city time, selling | skill-ups, gold | fast in combat-heavy play |
| roleplayer | in-character activity: shrines, inns, homeland, faction duty | story fit | medium |
| dungeon runner | LFG and instances | gear upgrades | fast while solo |
| casual | a balanced rotation, frequent rests | variety itself | medium |
| pvp-er | battlegrounds, outdoor PvP | honour, kills | medium |

Weights, reward sensitivities and boredom rates live in conf
(`OllamaChat.Autopilot.Playstyle.<name>.*`), so operators can add playstyles
without code changes. Assignment is weighted by conf, and the bot's existing
chat personality biases it. A `roleplayer` bot is also pushed towards RP mode
voice when `Roleplay.Enable` is on.

**Awareness** decides how the bot *thinks* about its goals, and so how the goal
text reads in chat:

- `immersed`: the character doesn't know it's a game. Goals are in-world: "earn
  enough coin to buy a proper mount", "see the great tree of Teldrassil".
- `player`: a person playing casually. "Level my mining, then dungeons with
  friends."
- `metagamer`: knows the systems and optimises. "Hit 40 by tonight; skip the
  escort quests."

The awareness level also chooses the persona block in the planner prompt. When
roleplay strictness is 2, only `immersed` is allowed.

## Progress history (DB)

New file `data/sql/characters/base/2026_10_05_autopilot.sql`, using
`CREATE TABLE IF NOT EXISTS`:

- `mod_ollama_chat_autopilot`: bot_guid PK, enabled, playstyle, awareness,
  current_activity, activity_since, goal_text, goal_kind, goal_target,
  goal_progress, goal_set_at, boredom, last_decision_at, last_reason.
- `mod_ollama_chat_autopilot_snapshots`: id, bot_guid, taken_at, level, xp,
  money, zone_id, area_id, map_id, durability_pct, free_bag_slots,
  quests_active, quests_completed_total, deaths_total, kills_total,
  activity, `professions` (compact `skill:value,...`). Taken every
  `SnapshotInterval` minutes and trimmed to `SnapshotRetention` rows per bot.
- `mod_ollama_chat_autopilot_events`: id, bot_guid, at, type (level, death,
  quest_complete, quest_accept, loot_rare, skill_up, zone_enter, goal_set,
  goal_done, goal_abandoned, activity_change, guard_fired), detail VARCHAR.
  Trimmed to `EventRetention` rows per bot.

Counters (kills, deaths, skill-ups since the last snapshot) are accumulated in
memory from the existing `events.cpp` hooks, which already fire for bots. They
are flushed with the snapshot. All DB writes are async, using the same pattern
as `memory.cpp`.

The prompt receives **deltas, not raw rows**: "last 2h: +1.4 levels, +38g, 11
quests, 2 deaths (both Stranglethorn), 0 skill-ups, 3 zones". That is concrete
and quotable, so it is what the model will reason about. That matches the
CLAUDE.md note on prompt content.

## Boredom, satisfaction and rewards

These are computed on the world thread from history. They are numbers, not LLM
output:

- **reward rate per channel**: xp/h, gold/h, quests/h, skill-ups/h, new
  zones/h, gear upgrades (from loot events).
- **satisfaction** = the reward rates weighted by the playstyle's sensitivities,
  normalised against that bot's own rolling baseline.
- **boredom** rises with time in the same activity and zone, faster when
  satisfaction is low, scaled by the playstyle's boredom rate. It drops when
  the activity changes or a goal completes.
- **temptations**: notable opportunities the bot noticed (a rare loot drop, a
  dungeon-level bracket reached, a profession trainer nearby, a new zone just
  unlocked by level). They are injected as short lines so the model can "get
  distracted" in character.

## The planner

This is the LLM side of the goal/policy split described under Scaling.
Foreground bots also get LLM activity choice; every other bot gets its
activities from the policy.

**When it runs.** A bot is due for an LLM decision, subject to the budget,
when any of these is true:

- `GoalRefreshMinutes` has passed (default 90). For foreground bots,
  `DecisionIntervalMinutes` (default 15) applies instead.
- its goal was completed or became impossible
- boredom crossed `BoredomThreshold`
- a major event happened (level-up into a new bracket, death streak, bags full,
  zone change by flight)
- a GM forced a replan

The scheduler submits at most `MaxConcurrentPlans`, and pauses entirely when
the dispatch queue is above half depth, so chat always wins.

**Prompt** (built on the world thread; every block is a conf template):

1. persona: name/race/class/level, playstyle block, awareness block, chat
   personality, and RP voice if that is on
2. the current goal and its progress, plus the current activity and how long it
   has been running
3. state: the existing `GenerateBotGameStateSnapshot` plus macro fields (gold,
   bag space, durability, professions, rested xp, level bracket)
4. history: progress deltas, the last N events, the last 3 decisions with their
   reasons, and the top memories from `memory.cpp`
5. feelings: boredom, satisfaction, temptations
6. menu: the activity names and one-line descriptions allowed *for this bot
   right now* (for example `dungeon` is listed only in a level bracket that has
   one), and the goal kinds
7. the output schema

**Output** (a JSON object, parsed with nlohmann on the worker):

```json
{
  "activity": "explore",
  "goal": { "kind": "explore_zone", "target": "Feralas",
            "text": "See the twin colossals before the season turns." },
  "keep_goal": false,
  "disposition": { "risk": "cautious", "greed": "greedy", "social": "reserved" },
  "playbook": { "dungeon": "cautious", "on_death": "wait_for_res" },
  "upkeep": ["repair", "sell"],
  "reason": "Bored of Desolace, nothing gained in an hour.",
  "say": "optional one-liner, delivered through the normal chat path"
}
```

The worker validates the shape. The world thread validates the meaning:
- `activity` must be on the menu
- `upkeep` items must be on the allow-list
- the goal kind must be known
- if the target is a zone or quest, it must resolve

On a validation failure the current plan is kept and a short-backoff retry is
scheduled. One bad generation never strands the bot.

**Activities** are data-driven presets in conf. The model only ever picks a
name:

```
OllamaChat.Autopilot.Activity.quest   = "nc:+quest,+new rpg,-grind;rpg:do quest"
OllamaChat.Autopilot.Activity.grind   = "nc:+grind,+new rpg;rpg:go grind"
OllamaChat.Autopilot.Activity.explore = "nc:+new rpg,+explore,-grind;rpg:wander random"
OllamaChat.Autopilot.Activity.travel  = "nc:+new rpg;rpg:travel flight"
OllamaChat.Autopilot.Activity.town    = "nc:+new rpg,-grind;rpg:wander npc;do:sell,repair,trainer"
OllamaChat.Autopilot.Activity.gather  = "nc:+gather,+new rpg;rpg:wander random"
OllamaChat.Autopilot.Activity.dungeon = "nc:+lfg"
OllamaChat.Autopilot.Activity.pvp     = "nc:+bg;rpg:outdoor pvp"
OllamaChat.Autopilot.Activity.rest    = "rpg:rest"
```

`nc:` maps to `ChangeStrategy(..., BOT_STATE_NON_COMBAT)`, `rpg:` to
`rpgInfo.ChangeTo*`, and `do:` to `DoSpecificAction`. Combat strategies are
never touched. NewRpg statuses time out back to IDLE on their own. The
applier re-asserts the preset's `rpg:` status on each sweep while the activity
is active, so a forced status doesn't decay into random wandering.
Goal-directed variants like `do quest <id>` or `go grind <pos>` use the
positioned `ChangeToDoQuest` / `ChangeToGoGrind` overloads with a target
resolved on the world thread (quest log, `sTravelMgr` destinations).

**Self-preservation guards** are deterministic and run before the LLM is
consulted:
- durability below X%: queue `repair`
- free bag slots below Y: `sell`
- N deaths in M minutes in one zone: force `rest` and replan with "you keep
  dying here" in the prompt
- level far below zone: replan with that fact
- stuck (no position or progress change for T minutes): replan

The LLM sees the guard events in its history, so the bot can comment on them in
character.

## Chat integration

The point of a goal is that the bot can talk about it. Add `{bot_goal}` and
`{bot_activity}` placeholders to the chat, event and random-chatter templates.
They are filled from the autopilot row and empty when the bot isn't enrolled.
The roleplay variation "Mention where you are headed and why" finally has a
true answer. A decision's optional `say` goes through `OllamaDispatch_Submit`
as a normal request, so the governor still gates it.

## Config surface (`conf/mod_ollama_chat.conf.dist`, new section)

`Autopilot.Enable`, `.AllowMasterEnroll`, `.Select.*`, `.MaxEnrolled`,
`.Model` (optional override, published via the endpoint snapshot),
`.LlmCallsPerHour`, `.GoalRefreshMinutes`, `.DecisionIntervalMinutes`,
`.ForegroundRange`, `.DormantAfterMinutes`, `.SweepIntervalMs`,
`.BotsPerSweep`, `.FlushIntervalSeconds`, `.BatchSize`, `.MaxConcurrentPlans`,
`.WithRealPlayer`, `.Disposition.*`, `.Playbook.*`, `.SnapshotIntervalMinutes`, `.SnapshotRetention`,
`.EventRetention`, `.BoredomThreshold`, `.Guard.*`, `.Activity.*`,
`.Playstyle.*`, `.Awareness.*`, `.PromptTemplate`, `.Debug`. Each one gets a
full comment block with its default.

## Commands

`.ollama autopilot on|off <bot>`, `preview`, `status <bot>` (playstyle, goal, activity,
boredom, last reason), `history <bot> [n]`, `goal <bot> <text>`,
`playstyle <bot> <name>`, `awareness <bot> <level>`, `replan <bot>`, `stats`
(global counts, decisions/h, validation failures).

## Phases

1. **Foundations, with no LLM control.**
   - SQL, config and playstyle profiles.
   - The `autopilot` strategy registration spike: verify that it shows up in
     `nc ?` for every class and survives `ResetStrategies`.
   - Selection rules, explicit on/off, `preview`.
   - Snapshot recorder and event log fed from `events.cpp`.
   - `status` and `history` commands.
   - Ships as "bots keep a diary".
2. **Policy first, then the planner MVP.**
   - The deterministic activity policy and preset applier come first, along
     with re-assert after reset and the guards. With just these, bots already
     act on their playstyle at zero LLM calls; that is the dormant tier.
   - Then `TaskType::Plan` plus an `OllamaRequestKind::Autopilot` entry with no
     think policy.
   - Prompt builder, JSON schema, parser and validator.
   - Tiers and the budget queue.
3. **Goals and feelings.**
   - Goal kinds with measurable progress against snapshots: reach level, reach
     skill, gold, explore zone, complete quest or chain, run dungeon.
   - Boredom, satisfaction, temptations, event-triggered replans.
4. **Chat and roleplay.**
   - `{bot_goal}` / `{bot_activity}` placeholders.
   - Awareness-specific phrasing; immersed goals filtered through
     `Roleplay_FilterMetaTerms`.
   - The `say` field.
5. **Beyond the existing strategies** (each needs new playbot actions,
   registered the same way as the strategy):
   - an auction-house posting action for merchants (world thread,
     `AuctionHouseMgr`)
   - grouping autopilot bots with matching goals
   - profession-training trips

## Open risks

- **Registration hack.** It depends on the class contexts staying public static
  in this fork. Mitigation: one isolated file, and a startup check that logs
  loudly if the strategy didn't register.
- **NewRpg weighting.** IDLE still picks a weighted random status, and
  re-asserting fights it. If that thrashes, the fallback is to set
  `rpgInfo` with long durations, or to drop `new rpg` for activities that
  have their own strategy.
- **Cost.** Cost is bounded by `LlmCallsPerHour`, not by bot count (see
  Scaling). The failure mode becomes "bots think too rarely" rather than
  "server overloaded", and `preview` and `stats` make that visible.
- **Small models and JSON.** Use Ollama `format: json` where the provider
  supports it. Otherwise fall back to the brace-extraction parser, made aware
  of strings, unlike bot-buddy's.
