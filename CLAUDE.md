# CLAUDE.md — mod-ollama-chat

Guidance for working in this module. The repo-level `CLAUDE.md` at the
AzerothCore root still applies; this adds module-specific rules.

## The one rule that matters most

**Worker threads do HTTP and string work only. Every read or write of a
`Player`, `Channel`, `Guild`, `Group` or `Map` happens on the world thread.**

This module talks to a network service, so it is permanently tempting to "just
do it on a background thread". Don't. AzerothCore's world state is not
thread-safe, and violating this produces intermittent crashes that are extremely
hard to attribute back here.

The module previously spawned one detached `std::thread` per bot per message and
called `ObjectAccessor`, `Channel::Say`, `botAI->Say` and the whole eligibility
scan from it. That is what `mod-ollama-chat_dispatch.{h,cpp}` exists to prevent.

How to add a new kind of bot utterance:

1. Build the prompt **on the world thread**, where the `Player*` is live.
2. Fill an `OllamaChatRequest` — resolve every value you need (names, guids,
   scope key) into it now, so the worker never has to touch a `Player`.
3. `OllamaDispatch_Submit(std::move(request))`.
4. Delivery happens for you in `OllamaDispatch_Update()`, on the world thread.

Do **not** call `QueryOllama()` directly from anywhere that could be the world
thread — it blocks for a full LLM round trip.

`EventProcessor::AddEvent` (`bot->m_Events`) is **not** a way around this. It
mutates a container that `Player::Update` walks, so calling it off-thread is the
same race it looks like it avoids.

## The second rule: workers never read config strings directly

A worker thread must not touch `g_OllamaUrl`, `g_OllamaModel`,
`g_OllamaSystemPrompt`, `g_OllamaStop`, `g_OllamaSeed`, `g_OllamaApiKey`,
`g_SentimentAnalysisPrompt` or any other `std::string` global.

`.ollama reload` reassigns those on the world thread. Reassigning a
`std::string` frees the old buffer, and a worker copying it at that moment
dereferences freed memory. This produced a real crash in the wild: an
ACCESS_VIOLATION deep inside cpp-httplib's header handling, with a stack that
pointed at the HTTP client rather than at the actual cause.

The pattern to follow:

- endpoint settings go through `OllamaConfig_Snapshot()`, published under a
  mutex by `OllamaConfig_Publish()` at the end of `LoadOllamaChatConfig()`
- anything else a worker needs is formatted on the world thread at submit time
  and carried in the task (see `BuildSentimentPrompt`,
  `Memory_BuildCondensationPrompt`)

POD globals (`bool`, `uint32_t`, `float`) are a benign racy read and are fine
to touch directly.

## Provider support lives in one file

`OllamaChat.Provider` (`ollama` / `openai` / `anthropic`) only changes how a
request is encoded and decoded inside `mod-ollama-chat_api.cpp`: one builder,
one parser and one header function per provider, selected on
`OllamaEndpointSettings::provider`. Nothing outside that file should branch on
the provider except the capability probe, which is skipped for non-Ollama
providers because a live probe there is a billed request. Add a provider by
adding a builder/parser pair and an enum value; do not teach callers about it.

## Channels are resolved by id, never by name

Zone channels are named `"General - Elwynn Forest"`, so
`ChannelMgr::GetChannel("General")` always returns nullptr. City channels
(Trade, GuildRecruitment) only exist in cities. Names are localized, so any
substring test on them breaks on a non-English realm.

Use `OllamaResolveZoneChannel(bot, ChatChannelId::X)`, which matches on channel
id plus zone-name containment the way `PlayerbotAI::SayToChannel` does, and
carry the channel's *actual* name and id into the request. Getting this wrong
silently loses ambient chatter, and also splits the governor's scope key so
ambient lines and replies in the same channel stop sharing a cooldown and a
repetition history.

## Script registration facts (verified against this fork)

- `PlayerScript(name)` with **no hook list enables every hook**. See
  `PlayerScript::PlayerScript` in the core: *"If empty - enable all available
  hooks."* Passing an explicit list is a performance nicety, not a requirement.
  Do not "fix" a bare `PlayerScript("Name")` thinking it registers nothing.
- Always write `override` on hook overrides. Two hooks in this module silently
  overrode nothing for a long time because they lacked it:
  - `OnPlayerCompleteAchievement` — the core's name is
    `OnPlayerAchievementComplete` (word order).
  - `OnGameObjectUse` — not a `PlayerScript` hook at all.
- There is no generic "player used a gameobject" hook. `ChatOnGameObjectUse`
  uses `AllGameObjectScript::CanGameObjectGossipHello` and returns `false` so
  normal handling continues.
- There is no `OnGuildMemberLogin`. Guild login uses `PLAYERHOOK_ON_LOGIN` plus
  a guild check; promote/demote come through `GuildScript::OnEvent`.
- `urand(min, max)` is **inclusive on both ends**. `urand(0, 100) > 0` fires
  about 1 in 101 times, so a configured chance of 0 is not "never". Use
  `urand(1, 100) > chance` and check `chance <= 0` first.

## Performance traps specific to this module

- Never walk `Map::GetCreatureBySpawnIdStore()` — it is every spawn on the map.
  Use `Cell::VisitObjects` with a searcher, as `topics.cpp` and `handler.cpp`
  now do.
- `Acore::AnyUnitInObjectRangeCheck` filters out anything not alive. Where dead
  creatures matter (corpse topics), use the local `NearbyCreatureCheck` structs.
- Watch for nested `ObjectAccessor::GetPlayers()` loops; that is quadratic in
  online characters, per message.

## Prompt content

Whatever is most concrete and quotable in the prompt is what the model will
talk about. Historically the module pasted every off-cooldown spell a bot knew
into the prompt, which is exactly why bots kept talking about their spellbook.
`OllamaChat.Snapshot.IncludeSpells` defaults to `0` for this reason.

When adding prompt material, prefer things outside the bot — people nearby,
what just happened, where they are — over facts about the bot itself. The
weights in `OllamaChat.Topic.*` encode this deliberately.

## Conventions

- Log to `module.ollamachat`, not `server.loading`. It falls back to the
  `Logger.module` entry that ships in `worldserver.conf.dist`, so it works
  untouched and is separately tunable. Keep startup/registration messages on
  `server.loading`.
- Every new setting goes in `conf/mod_ollama_chat.conf.dist` with a comment
  block explaining what it does and its default. That file is the module's real
  documentation surface.
- Source files are globbed by AzerothCore (`modules/*/src/*.cpp`); new files
  need no CMake change.
- New tables go in `data/sql/characters/base/` and must be
  `CREATE TABLE IF NOT EXISTS`.

## Checking your work without a full build

A full AzerothCore build is slow. For a syntax + semantic check of just this
module, `cl /Zs` against the collected include paths works and catches
signature mismatches, missing declarations and bad `override`s:

- include dirs: every subdir of `src/common` and `src/server`, every deps dir
  containing headers **plus its parent** (fmt needs `deps/fmt/include`),
  all of `modules/mod-playerbots/src`, this module's `src` and `deps`, plus
  boost, the MySQL include dir and `var/build/obj` for `revision.h`
- exclude `deps/g3dlite/include/G3D` — it has `Log.h`, `Random.h` and
  `Spline.h` that shadow AzerothCore's
- exclude `deps/jemalloc` — its internal `Util.h` shadows AzerothCore's
- needs `/std:c++20 /utf-8 /FI"Log.h"` (several core headers assume the PCH has
  already pulled `Log.h` in)
- pass the flags in a response file; ~490 include paths overflow the command
  line

This does not link, so it will not catch a declared-but-undefined function.

## Autopilot (the LLM as the bot's master)

**The LLM makes the decisions; code never does.** The model writes each bot's
identity, sets its goals, and gives orders the way a player whispers to their
own bot: playerbots chat commands plus autopilot's own `goto` and `quest`.
Code reports facts and runs the orders. Playerbots' own overhead controllers
(`new rpg`, `rpg`, `travel`) are kept off on enrolled bots and the model may
not turn them on. NewRpg is exactly the controller the LLM replaces, so never
route autopilot behaviour through it. Do not add preset playstyles,
weight tables, dice-roll policies, formula-driven moods or code that "helps" by
acting on a condition itself (alerts ask the model early; they never act). Two
earlier versions did: one picked activities from playstyle weights, one only
toggled an allow-listed set of strategies and was indistinguishable from plain
NewRpg. Both were the opposite of what the feature is for. When the model
cannot be asked, a bot keeps its last orders rather than code making some up.

This is a module-only feature: it must never require changes to mod-playerbots
or the core.

Design: `docs/autopilot-plan.md`. Facts that are easy to get wrong:

- **Orders run through `PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER, text,
  bot)` with the bot as sender.** Playerbots' security accepts a bot
  commanding itself, and the command queues into the bot's own update like a
  master's whisper. Commands that need a master to reply to (`rpg status`) or
  a master's target (`trainer`, `home`, `rpg do quest` without a link) do not
  work this way. That is why `goto` (with `Trainer::TeachSpell` and
  `SetHomebind` on arrival) and `quest <id>` (turn-in through playerbots'
  `talk to quest giver` action with the bot targeting the NPC) exist in
  `mod-ollama-chat_autopilot_commands.cpp`.
- **`nc`/`co` orders do not go through `HandleCommand`.** The chat path calls
  `PlayerbotRepository::Save`, which writes the model's choices into
  playerbots' store as if a master had made them. They go to
  `ChangeStrategy` directly and are recorded in `Row::strategies`, which is
  replayed after every playerbots reset.
- **Registering a playerbots strategy from this module** works by adding a
  `NamedObjectContext<Strategy>` to all ten class contexts'
  `sharedStrategyContexts` (public statics; see
  `mod-ollama-chat_autopilot_strategy.cpp`). Do it at `OnStartup`, before any
  bot logs in, because bots read those creator maps on map threads. Keep
  strategy-engine and movement-value includes confined to that file.
- **The marker strategy is the reset detector**, one per engine. Missing
  means playerbots reset that engine: put the model's strategies back. Never
  treat a strategy's absence as anyone's intent.
- **Never undo a strategy change by inverting it.** Playerbots defaults include
  `potions`, `chat`, `loot`, `gather` and `emote`. Before the model's first
  change, both engines' full lists are captured (`Row::baseline`), and
  `HandBack` restores exactly that.
- **Playerbots persists the marker.** `PlayerbotRepository::Save` writes every
  strategy, `autopilot` included, when a grouped bot logs out, and `Load`
  restores them at login. A marker present at login is not a master's
  request; only one that appears mid-session is. `ResetStrategies` does not
  reload the repository; `HandBack` does, for alts.
- **Random-bot teleports are held per bot through public
  `RandomPlayerbotMgr` calls only:** `ScheduleTeleport(low, 2h)` hourly,
  `SetValue(low, "randomize", 1)`, and for a death `SetValue(low, "dead"/"revive",
  1)` set *before* `ProcessBot` marks the death itself (otherwise its own
  1–5 minute revive timer overwrites ours). Clear both when the bot is alive
  again, or the next death is revived (and teleported) at once.
- **Autopilot moves bots itself** (`AutopilotMove_To`): a `MovePoint`
  recorded in the bot's `"last movement"` value at `MOVEMENT_NORMAL`, so
  playerbots' own out-of-combat movement waits and combat still outranks it.
  Holds on a deck use `MOVEMENT_FORCED`.
- **Long walks follow a navmesh route** (`mod-ollama-chat_autopilot_route.cpp`,
  ported from mod-city-siege's `CitySiegePathing.cpp`), one node (~28 yd) at a
  time. The route is built lazily, a few queries per visit and a
  few hundred yards ahead, because mmap tiles load only with their grid: a
  query far ahead of the bot can fail just because the tile is not loaded, so
  a failure counts only once the bot is near it.
- **Travel** (`mod-ollama-chat_autopilot_travel.cpp`) plans legs and replans
  after every flight or crossing. Flights call `ActivateTaxiPathTo` directly
  with a path from TravelMgr's flight-master cache and `FindTaxiPath`; this
  core needs no known nodes, only money. Boats and zeppelins come from
  `TransportMgr` templates (stop key frames) at startup, with docks judged
  friendly by nearby creature factions. Boarding relies on
  `PlayerbotAI::UpdateAI` attaching a bot to the transport under it every
  second. `MotionTransport::IsMoving` is private, so "docked" means near the
  stop and not moving since the last look.
- **PlayerScript progress hooks run on map threads**, several at once. They
  may read only the player they were handed plus mutex-guarded module state.
  `Autopilot_Update` runs in `WorldScript::OnUpdate`, after `MapMgr::Update`
  has joined its workers, so it may touch any bot and call
  `RandomPlayerbotMgr`. Never call `RandomPlayerbotMgr` from a hook.
- **Keep per-tick cost flat.** The sweep visits a fixed `BotsPerSweep` in
  rotation. Snapshots are staggered by guid hash, and DB writes are batched
  per flush. The service index (`AutopilotWorld_Build`) is built once at
  startup from `GetAllCreatureData`; never walk spawns per order.
- **The planner's prompt template is filled by literal `{name}`
  replacement, not fmt**, so JSON braces in it need no escaping. Free text
  from the model has its braces neutralised before it is fed back in.
