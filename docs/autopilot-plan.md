# LLM Autopilot — design

Branch: `feature/llm-autopilot`

## Principle

**The LLM makes every behavioural decision. Code never does.**

For each enrolled bot, the LLM decides:
- who the character is: an identity it writes itself
- what the character is working toward: measurable goals
- what the character does about it: which existing playerbots strategies are
  on, what it focuses on, and how it behaves in each situation

Playerbots' own AI does the walking and fighting.

Code does four things only:
1. Reports facts to the LLM.
2. Enforces an allow-list of strategies.
3. Applies a few safety guards.
4. Carries out the LLM's choices.

When the LLM cannot be asked, a bot keeps the plan the LLM last gave it. Cost
controls decide *when* the LLM is asked, never *what* a bot does.

An earlier draft went the other way: preset playstyles, activity weight
tables, a dice-roll fallback policy and a boredom formula. It was removed for
that reason. Do not reintroduce code-side decision logic.

## Data flow

```
 world thread (Autopilot_Update, after MapMgr::Update)        worker pool
 ┌──────────────────────────────────────────────────┐
 │ sweep: fixed budget of enrolled bots per tick     │
 │  ├ facts: diary snapshot, zone, gold/skill, goal │
 │  ├ guards (safety only)                          │
 │  ├ Reassert: marker, allow-list, situation,       │
 │  │   apply the LLM's wanted strategies           │
 │  ├ plan due? → build prompt → Submit ────────────┼─► QueryOllama(Autopilot)
 │  └ SteerRpg: keep the bot in the LLM's focus     │     parse JSON
 │ drain decisions ◄────────────────────────────────┼── AutopilotDecision
 │  └ ApplyDecision: validate, store, apply now     │
 └──────────────────────────────────────────────────┘
 PlayerScript hooks (map threads): counters, events, temptations (under g_mutex)
```

## What the LLM decides (one JSON reply)

| Field | Meaning | Applied as |
|---|---|---|
| `identity {style, outlook, profile}` | Who the character is. Required on the first plan; revised only when it really changes | Stored; shown in every later prompt |
| `doing` | Its own label for the current plan | Shown in status and prompts |
| `strategies ["+quest", "-grind", "+flee"]` | Behaviours to switch; unmentioned ones keep their state | Merged into the bot's wanted set (allow-listed names only) |
| `rpg ["do quest", "wander npc"]` | NewRpg focus; `[]` lets the bot roam | Steered with playerbots' own target selection |
| `minutes` | When to ask again (10–180) | `plan_until` |
| `goal {kind, target, text}` | Measurable aim | Resolved against game data; progress measured from the bot |
| `playbook {situation: ["+x"]}` | Combat strategies for `dungeon`, `battleground` or `with_player` | Applied on entering, reverted on leaving |
| `reason` | Why, in its own terms | Diary and later prompts |

## What code reports to the LLM

- The character: race, class, level and guild, plus the module's chat
  personality, if any.
- Its identity, current plan, goal and progress, and live strategies with
  descriptions.
- History:
  - recent decisions
  - recent events
  - progress across snapshots
  - last-hour rewards, as plain counts
  - temptations (a door just opened, an epic drop, a capped profession, a
    goal just achieved)
- Surroundings (for foreground bots) and memories.
- Strict roleplay realms add "must be in character".

## Applying the LLM's choices

- **Allow-list** (`Strategies.NonCombat`, `.Combat`, `RpgStatuses`):
  - Names are checked against the engine playerbots actually reads them from.
  - Narrowing the list stops enforcing earlier choices at once.
- **Situations:**
  - Out-of-combat strategies and the focus apply only while the bot is its
    own: not in a human's group, not following a bot group, not in an
    instance or battleground.
  - Combat strategies and the playbook apply everywhere.
  - `WithRealPlayer = 0` means hands off entirely in a human's group.
- **Focus needs `new rpg`:** a focus turns `new rpg` on and legacy `rpg`
  off, unless the LLM itself turned `new rpg` off.
- **Baseline:**
  - Each touched strategy, and the siblings playerbots drops when it is
    added, is recorded as it was before autopilot, once, in the database.
  - A strategy that stops being managed goes back to its baseline.
  - Hand-back (turning a bot off, autopilot off, `Control = 0`) resets the
    bot, reloads an alt's saved strategies, then restores the baseline. That
    undoes anything playerbots' own store captured from us.
- **Reset detection:**
  - A marker strategy (`autopilot`) sits on both engines.
  - If one goes missing, playerbots reset that engine, and our choices for it
    are re-applied.
  - If something we set changes while the marker stays and a human is in the
    group, the human did it, and that strategy is left to them until the
    group breaks up.

## When the LLM is asked

A bot's plan is due when:
- its `plan_until` has passed
- it has no identity yet
- an event happened: a level, a goal done or stalled, leaving a
  dungeon/battleground/group, or landing from a flight

Two limits apply:
- **Tier spacing:**
  - foreground (a player near, or a guildmate online): `DecisionIntervalMinutes`
  - background (a player on the map): `GoalRefreshMinutes`
  - dormant: never
  - `ForegroundScope`, `BackgroundScope`, `MinimumTier` and
    `RealPlayerGuildTier` widen this.
- **Event gap:** events may ask sooner, but at most once per 5 minutes.

Each plan spends one token from `LlmCallsPerHour`, and `MaxConcurrentPlans`
caps plans in flight. A plan is not built at all while the shared queue is
past half full, so chat comes first. `plan_until` and `last_plan_at` persist,
so relogging does not trigger a new plan.

## Safety guards (the only behaviour code decides)

| Condition | Override |
|---|---|
| A death streak | rpg focus `rest` |
| Durability below the threshold | `wander npc` / `go camp` |
| Bags nearly full | `wander npc` / `go camp` |

Each override lasts `Guard.HoldMinutes`. The reason goes into the next prompt.
Each guard then stays quiet for four holds, so an unfixable condition cannot
loop.

## Selection, diary, scale

- **Selection** (once per login and reload):
  - Sources: random-bot percent, real-player guilds, guild/account ids,
    include/exclude names, alts, a master's `nc +autopilot`, GM on/off.
  - `MaxEnrolled` caps the rule-based sources.
  - Deleted characters are removed: in the delete transaction, and by an
    orphan sweep at startup and hourly.
- **Diary:**
  - Staggered snapshots and notable events, written in batches.
  - Snapshots are thinned hourly after a day and daily after a week.
- **Sweep:** a fixed number of enrolled bots per tick, so per-tick cost
  doesn't grow with the bot count.

## Threading

Hooks run on map threads and touch only their own player plus state under
`g_mutex`. Everything else is on the world thread. The planner job does HTTP
and string work only.

## Possible next steps

- Feed the bot's goal and plan into its chat prompts, so it can talk about
  what it's doing.
- A `with_player` mode where the bot voices what it wants in party chat.
- New playerbots actions for things no strategy covers yet, such as posting
  auctions.
