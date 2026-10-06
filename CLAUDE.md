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
- **Autopilot moves bots itself** (`AutopilotMove_To`): a navmesh path walked
  with `MoveSplinePath` (an escort spline), recorded in the bot's
  `"last movement"` value at `MOVEMENT_NORMAL`, so playerbots' own
  out-of-combat movement waits and combat still outranks it. Holds on a deck
  use `MOVEMENT_FORCED`.
- **Never loosen the core's bot navmesh filter.** `PathGenerator::CreateFilter`
  gives headless (bot) sessions no steep slopes, no lava/slime and water at
  20x cost. Calling `SetExcludeFlags` replaces those exclusions -- once that
  let bots climb mountainsides. Every autopilot path goes through
  `AutopilotRoute_Filter`. `SetSlopeCheck(true)` is tried first. The core
  cuts a slope-checked path at the first step it finds too steep, a river
  bank included, and a bot handed that stub stood at the bank for good. So
  when it gets nowhere, a retry without the slope check is taken **only if**
  `AutopilotRoute_ClimbsOnlyWhereWet` passes: every step that fails the
  core's own `PathGenerator::IsWalkableClimb` must touch water. An
  unchecked retry put bots back on mountainsides; the navmesh's
  `NAV_GROUND_STEEP` only marks the very steepest polygons.
- **The walk waits for casts.** Playerbots never starts a spell with a cast
  time while the bot moves, and a move order cancels a cast in progress. So
  `Control` holds the walk while the bot casts (and 2 s after, for a chained
  buff), and stops a long walk for 2 s every 30 s so upkeep such as a pet
  summon gets its moment. That is movement pacing; what to do is still the
  model's and playerbots'.
- **Autopilot's walk yields to combat.** It runs as an escort spline that a
  caster cannot cast through; any visit (and a per-sweep check for walking
  bots) that finds the bot attacked stops it (`AutopilotMove_Yield`).
- **Never `MovePoint` with pathfinding for autopilot moves.** When the
  navmesh has no path it silently moves in a straight line (through walls, up
  cliffs). `AutopilotMove_To` computes the path itself and walks it with
  `MoveSplinePath`, or does not move at all. The one exception is a step of
  at most 8 yd, in line of sight and without a climb (onto a portal, off a
  slope the filter excludes). A two-point path is padded to three, because
  the escort generator hands two points to `MoveTo` with pathfinding on.
  Long routes (150 yd and up) are anchored on playerbots' road network: the
  three nearest travel nodes within 600 yd of each end (nodes are sparse in
  open country; 200 yd found none, so walks went straight over the hills),
  `TravelNodeMap::getRoute(node, node)` under our own `try_to_lock` on
  `m_nMapMtx`, followed up to the first link that isn't a walk.
  `AutopilotRoute::anchorNote` says why a route has no anchors, and the
  monitor's Travel page shows it. **Never call `getFullPath`**: it returns
  with the shared lock still held when it finds no route, and allocates a
  node per call. Corridors never aim at an unloaded tile (the core answers
  those with a NOT_USING_PATH shortcut), and an anchor a corridor cannot
  get closer to is skipped. A query that cannot start because the bot stands
  on a steep slope is not "no mmaps": the route steps off onto walkable
  ground nearby first.
- **Long walks follow a navmesh route** (`mod-ollama-chat_autopilot_route.cpp`,
  ported from mod-city-siege's `CitySiegePathing.cpp`), one node (~28 yd) at a
  time. The route is built lazily, a few queries per visit and a
  few hundred yards ahead, because mmap tiles load only with their grid: a
  query far ahead of the bot can fail just because the tile is not loaded, so
  a failure counts only once the bot is near it. For a player source, the
  core answers a query ending off the mesh (inside a wall) exactly like one
  ending on an unloaded tile (NOT_USING_PATH). So corridor targets and
  shortened steps are snapped onto walkable ground first (`SnapToMesh`,
  Detour's `findNearestPoly` on the map's own query object, world thread),
  and `TileLoaded` tells the two failures apart. Without that, a bot inside
  the Exodar never got a route out.
- **Travel** (`mod-ollama-chat_autopilot_travel.cpp`) plans legs and replans
  after every flight or crossing. Flights call `ActivateTaxiPathTo` directly
  with a path from TravelMgr's flight-master cache and `FindTaxiPath`. The
  core does not check the taxi mask, so autopilot does: only discovered nodes
  (`m_taxi.IsTaximaskNodeKnown`) are flown to, and `goto flightmaster`
  discovers one. Crossings between continents are
  indexed at startup: boats and zeppelins from `TransportMgr` templates (stop
  key frames), the Dark Portal from `GetAllAreaTriggerTeleports`, and city
  portals from spellcaster/goober gameobjects whose spell has a
  `SPELL_EFFECT_TELEPORT_UNITS` target position. A bot never sends the
  area-trigger packet, so walking into the Dark Portal copies the core's
  `HandleAreaTriggerOpcode` (`PlayerCannotEnter`, then `TeleportTo`). A
  faction owns a dock or portal when its NPCs are hostile to the other side
  only (`IsFriendlyTo` misses neutral ports such as Booty Bay; monsters are
  hostile to both and count for nobody). Boarding relies on
  `PlayerbotAI::UpdateAI` attaching a bot to the transport under it every
  second. `MotionTransport::IsMoving` is private, so "docked" means near the
  stop and not moving since the last look.
- **An NPC is reached, not approximated.** The final `Approach` leg is a
  `Walk` (with its route, through doors and up stairs) aimed at where the
  NPC stands, and it is done only within `kTouch`. On arrival, an errand with
  an NPC that is further than 8 yd away reports "could not get to X: stopped
  N yd away" instead of acting from the yard. The old approach made one path
  attempt and called a timeout "close enough". `goto <name>` (errand kind
  `Npc`, names indexed from creature spawns) talks to the NPC on arrival
  through `AutopilotQuest_TalkTo`.
- **Finding nothing is not the end of looking.** `goto hunt` with no known
  hunting ground, and a quest hunt with no known spawn left, explore:
  `ExploreNext` walks ~120 yd to walkable ground (`AutopilotRoute_WalkableNear`
  snaps onto the navmesh) and turns 135 degrees per leg, so the legs spiral
  out. After each `goto hunt` leg, `AutopilotWorld_PreyNear` (90 yd, live,
  hostile to the racial faction, level -8..+2, not in combat) retargets the
  trip at what it finds. The legs are capped (6 and 4), so a bot reports
  "found nothing" to the model instead of wandering forever.
- **Bots at a dock or aboard are stepped every sweep** (`g_aboard`), outside
  the rotation: a ship docks for well under a minute.
- **`g_mutex` in `autopilot.cpp` is recursive on purpose.** The sweep holds it
  while acting on the world (a quest turned in, a spell learned), and the core
  fires our own progress hooks for that on the same thread (level up, rare
  loot, achievements), which lock it again. A plain mutex there crashed or
  hung the server. Hooks must keep to updating existing entries, never
  inserting or erasing, so the sweep's references stay valid.
- **Orders run in the order the model gave them.** `RunCommands` starts the
  first `goto`/`quest` and queues every later order except `nc`/`co` in
  `Online::errandQueue` (so "goto vendor, b vendor" buys at the vendor);
  `StepErrand` runs the next when a trip ends, and asks the model again only
  when the queue is empty. Running them all at once made each trip replace
  the last, so only the final order ever happened. A plan with a trip
  replaces the queue.
- **A trip holds grind.** The marker strategy's multiplier zeroes grind's
  `attack anything` and `move random` while the bot is in
  `AutopilotStrategy_SetTravelling`'s set. `Control` refreshes that set on
  every visit and only adds a bot that is on its own with an errand under
  way, so a stale entry can never stop a grouped bot grinding in a dungeon.
  Without the hold, grind pulled neutral mobs and wandered, and the walk lost
  every time.
- **A failed crossing is avoided, not fatal.** `AutopilotTrip::crossing` is
  the crossing the current plan uses. When any leg fails while the bot is
  still on that side, it goes into `avoid` and the trip replans (bounded by
  `kMaxPlans`), so a dead custom portal falls back to the boat.
- **Quests that are not the bot's.** A random bot can hold quests for
  another class or race. The prompt marks them, `quest` refuses them
  (`SatisfyQuestClass`/`SatisfyQuestRace`), and `abandon <id>` drops one
  through `HandleQuestLogRemoveQuest` (playerbots' `drop` needs a master).
  Finished quests show who takes them and where (`DescribeQuestEnder`).
- **A quest objective is worked, not just visited.** `NeedsOf` lists what a
  quest still needs: creatures to kill (`RequiredNpcOrGo > 0`), objects to
  use (`< 0`), and the creatures and objects that give missing items
  (`creature_questitem` / `gameobject_questitem`, reverse-indexed in
  `AutopilotWorld_Build`, which also indexes spawn points of quest
  gameobjects only). An object to use is used through
  `HandleGameObjectUseOpcode` + `HandleGameobjectReportUse`, like the client.
  An object holding an item is opened now by `WorkObject`. It points
  playerbots' `"loot target"` at it and runs `DoSpecificAction("open
  loot")`, which picks the gathering spell, key or opening spell for the
  lock. A lockless object is used through the client's packets instead.
  Once `GetLootGUID()` is the object, `TakeOpenLoot` takes the coin and
  items and releases. Three failed tries put the object in
  `objectsGivenUp`. `open <object>` works the same way. Do not just add
  objects to the loot stack: playerbots' looting runs on its own schedule,
  and the hunt moved on first. A `quest` errand with anything needed turns
  into a hunt on arrival (`errand.hunting`, `Hunt` in commands.cpp). For
  creatures: The nearest needed creature that is alive,
  attackable, untapped and in sight goes into playerbots'
  `"prioritized targets"`, which `AttackersValue` counts as an attacker, so
  grind and the combat engine take it first. Within 25 yd,
  `DoSpecificAction("attack anything")` starts the fight, even with grind
  off and past the travelling hold. With none in sight, it walks to the next
  spawn of a needed creature at least 35 yd away. It ends when the
  objective is done or after `QuestHuntMinutes`, and the target is cleared.
  Grind on its own takes the *nearest* mob; quest need only counts for its
  out-of-range picks while rpg is active, which autopilot turns off.
- **Hostility uses the racial faction** (`OwnFaction`: `sChrRacesStore` ->
  `FactionID`), not `GetFactionTemplateEntry()`. GM mode sets the current
  faction to 35, friendly to everything, and a GM-mode bot then found no
  monsters to hunt.
- **Professions run on playerbots where it can, ours where it cannot.**
  Gathering, corpse looting and skinning are playerbots' `gather` and `loot`
  (`LootObject` reads the node's lock for the skill, and checks for a
  skinning knife). Fishing is `master fishing`, which needs no master.
  Crafting is ours: `craft <recipe>` (errand kind `Craft`) finds a known
  create-item spell on a skill line, walks to the nearest station of the
  recipe's `RequiresSpellFocus` (spell-focus gameobjects indexed at
  startup), then casts it once per visit via `PlayerbotAI::CastSpell` until
  the count or the materials run out. Playerbots' own `craft` needs a
  master. `disenchant` casts 13262 on a bag item. `open <object>` puts a
  chest or node into the loot stack, or uses the object with the client's
  packets. The prompt lists craftable recipes
  (`AutopilotCommands_DescribeCraftable`) and what is around the bot
  (`AutopilotCommands_DescribeSurroundings`: bodies, nodes with the skill
  each needs, chests, fishing pools, stations).
- **The bot loots its kills itself.** Playerbots' looting runs only out of
  combat, on its own schedule, and the quest hunt (or a trip) engaged the
  next target first, so bodies, and the quest items on them, were left.
  `LootBodies` in autopilot.cpp runs on every visit, 1 s after combat and
  before `StepErrand`. It walks to the nearest body within 25 yd that
  `isAllowedToLoot` and isn't looted, then calls `HandleLootOpcode`, then
  `HandleLootMoneyOpcode` and `HandleAutostoreLootItemOpcode` per slot
  (`GetMaxSlotInLootFor`), then `HandleLootReleaseOpcode`. Each body is
  tried once (`Online::lootTried`), so full bags never loop.
- **Auction house and mail use the client's handlers.** `ah sell`/`ah buy`
  build `CMSG_AUCTION_SELL_ITEM` / `CMSG_AUCTION_PLACE_BID` and call
  `HandleAuctionSellItem` / `HandleAuctionPlaceBid` with an auctioneer
  within reach, so the core charges the deposit and cut. Listings are read
  from `sAuctionMgr->GetAuctionsMap(auctioneer faction)`. Everything the
  auction house sends arrives by mail, so `goto mailbox` (mailbox
  gameobjects indexed at startup) takes money and items through
  `HandleMailTakeMoney` / `HandleMailTakeItem`, skipping cash-on-delivery.
  Without it, purchases would never reach the bags.
- **Crafting checks before it casts.** `MissingTool` (`SpellInfo::Totem` and
  `TotemCategory`; a category is named by the first item of it) and
  `StationNear` (a spell-focus object of the recipe's focus id within its
  `spellFocus.dist`) run before every cast. A craft counts only when the
  item count in the bags rises (`craftCasting`/`craftHad`).
- **A player's choices are the model's.** Quest rewards (`quest <id> reward
  <n>`, taken with `RewardQuest` before playerbots' turn-in, which would pick
  by stat weights), professions (`goto profession <name>`; a trainer's skill
  is read from what it teaches, and a bare `goto profession` asks which),
  talent specs (`talents spec <name>`; a bare `talents` only prints help).
  The prompt lists what each choice needs: reward choices under a finished
  quest, free profession slots, unspent points and the class's specs. The
  command reference in `AutopilotCommands_Reference` is the model's whole
  documentation of its orders: keep it true to what the code does. It once
  offered `autogear` (which conjures gear) and a `talents` that did nothing.
- **Enrolled bots earn everything (`NoHandouts`).** Playerbots hands random
  bots things outside any strategy. The marker strategy's multiplier
  (`AutopilotHoldMultiplier`) returns 0 for `auto maintenance on levelup`,
  `random bot update` and `lfg accept`, and, when the bot is alone in the
  open world, for `auto release`, `release` and `spirit healer` (free repair,
  sickness-free res). A zero multiplier makes `Engine::DoNextAction` drop the
  action; it does not stop `DoSpecificAction`, so never call those yourself.
  The marker is on all three engines (dead too) for this. Also: the taxi
  cheat is cleared in `Reassert`, "death count" is zeroed each visit (five
  deaths trigger playerbots' re-roll revive), the bot releases through
  `HandleRepopRequestOpcode`, falls back to `SendSpiritResurrect` after
  CorpseRunMinutes, dungeon-finder proposals are accepted by
  `AutopilotServerScript::OnPacketSent` + `AnswerLfgProposals`, and
  `maintenance` / `autogear` / `bis` / `cheat` orders are refused. Global
  `AiPlayerbot.BotCheats` (food) and playerbots' level reset at the cap
  (`AiPlayerbot.ResetBotLevel.*`, which ignores friend lists) cannot be
  lifted per bot from here.
- **The level-bracket shield is a `character_social` row.** Playerbots'
  `RandomBotLevelMgr` (`AiPlayerbot.LevelBrackets.*`; the old standalone
  brackets module is no longer used) skips a bot that is anyone's friend
  (`flags = 1`) while `IgnoreFriendListed` is on (the default).
  `SyncBracketShield` writes `(guid 0, friend = bot, flags 1, note 'ollama
  autopilot')` for enrolled bots as their rows save, deletes it when they
  leave, and `Autopilot_Load` rebuilds all of ours at startup. Guid 0 is no
  character, so the row is on nobody's list. Only rows with that note are
  ever ours to touch.
- **Playerbots' re-roll and refresh empty the bags.** `Randomize` and
  `Refresh` call `ClearInventory()`. Their timers are held from `OnStartup`
  (`Autopilot_Load`) for every enrolled random bot, not only from the sweep:
  a bot whose timers lapsed is re-rolled 3-8 s after it logs in. `"add"` is
  written only for random-bot accounts (`IsAccountType(account, 1)`); on an
  alt it would make playerbots log the alt in as a random bot.
- **Every order goes through `AutopilotCommands_Normalize` and
  `AutopilotCommands_IsDenied`.** The deny check refuses playerbots' command
  separator and `#` prefixes, which `HandleCommand` would otherwise split or
  strip past a leading-words check.
- **The autopilot tables are repaired at startup**
  (`mod-ollama-chat_autopilot_schema.cpp`): missing tables created, missing
  columns added, short VARCHARs widened. A column the module does not know is left alone
  unless it is NOT NULL with no default (it would fail every save); names
  are compared without case.
  The core's updater runs the SQL file once per content change and only as
  `CREATE TABLE IF NOT EXISTS`, so it never fixed a table an earlier draft
  made. Change a column in both places.
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

## Passer-by buffs (`mod-ollama-chat_buffs.cpp`)

World thread, from the module tick, a fixed `BotsPerSecond` in rotation. Buffs
are found in each bot's spellbook by shape, not by spell id: instant, at
least 10 minutes, no reagents, every effect an aura on `TARGET_UNIT_TARGET_ALLY`
of a stat, armour, resistance, attack power, regen, health or damage-shield
type. That excludes self-only and group buffs without a list to keep up.
Casting goes through `PlayerbotAI::CanCastSpell` / `CastSpell`. A cast that
will not go off on someone is remembered for `retrySeconds`, so a bot does
not retry it every pass.

## Monitor addon (`addon/OllamaMonitor`, `mod-ollama-chat_monitor.cpp`)

A GM-only debug window. The client whispers itself on the addon channel
(`OAPM\t<request>`); `OllamaMonitorScript::OnPlayerCanUseChat` takes it on
the world thread and answers with addon whispers. Every page is formatted on
the server as plain lines ("# Title", "key: value"). The client only colours
and shows them, so a new fact needs no client change.

- **State read for a page is copied out under its owner's mutex** through an
  accessor (`Governor_GetBotDebug`, `Memory_GetDebug`, `Topics_GetDebug`,
  `OllamaDispatch_GetTrace`, `AutopilotPlanner_LastExchange`,
  `Autopilot_MonitorPage`). Never keep references across the lock.
- **The chat trace and planner exchange are written by workers**, under
  their own mutexes only. They are capped by bot count (256 and 48) so a
  realm full of bots cannot grow them.
- **The camera is `Player::SetViewpoint`, and it holds a raw pointer**
  (`m_seer`) to the bot. It must be released before the bot leaves the
  watcher's map or the world. `OnPlayerBeforeTeleport` releases it on the
  map thread updating the bot, which is also the one updating any watcher on
  that map. `OnPlayerLogout` releases it on logout. `Monitor_Update`
  re-applies it, and teleports the watcher along when the map differs, but
  never into an instance (`TeleportTo` cannot pick the bot's instance id).
- **A stale `PLAYER_FARSIGHT` locks the camera.** `GetViewpoint` only finds
  the seer on the watcher's own map, and the core refuses any new viewpoint
  while the field holds a guid (`AddGuidValue`). So `ReleaseView` clears the
  field, the seer link and the shared-vision entry itself when the seer is
  elsewhere. From a map thread it defers that to `Monitor_Update`. `UNWATCH`
  always releases and always answers, and `GOTO` ends the follow first, or
  the follow tick pulls the watcher back.
- Personality is read from `g_BotPersonalityList` directly, because
  `GetBotPersonality` assigns and saves one when missing.
