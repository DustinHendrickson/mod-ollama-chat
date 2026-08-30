<p align="center">
  <img src="./icon.png" alt="Ollama Chat Module" title="Ollama Chat Module Icon">
</p>


# AzerothCore + Playerbots Module: mod-ollama-chat


> [!CAUTION]
> **LLM/AI Disclaimer:** Large Language Models (LLMs) such as those used by this module do not possess intelligence, reasoning, or true understanding. They generate text by predicting the most likely next word based on patterns in their training data—matching vectors, not thinking or comprehension. The quality and relevance of responses depend entirely on the model you use, its training data, and its configuration. Results may vary, and sometimes the output may be irrelevant, nonsensical, or simply not work as expected. This is a fundamental limitation of current AI and LLM technology. Use with realistic expectations.
>
> This module is also in development and can bog down your server due to the nature of running local LLM. Please proceed with this in mind.

> [!IMPORTANT]
> To fully disable Playerbots normal chatter and random chatter that might interfere with this module, set the following settings in your `playerbots.conf`:
> - `AiPlayerbot.EnableBroadcasts = 0` (disables loot/quest/kill broadcasts)
> - `AiPlayerbot.RandomBotTalk = 0` (disables random talking in say/yell/general channels)
> - `AiPlayerbot.RandomBotEmote = 0` (disables random emoting)
> - `AiPlayerbot.RandomBotSuggestDungeons = 0` (disables dungeon suggestions)
> - `AiPlayerbot.EnableGreet = 0` (disables greeting when invited)
> - `AiPlayerbot.GuildFeedback = 0` (disables guild event chatting)
> - `AiPlayerbot.RandomBotSayWithoutMaster = 0` (disables bots talking without a master)

## Overview

***mod-ollama-chat*** is an AzerothCore module that enhances the Player Bots module by integrating external language model (LLM) support via the Ollama API. This module enables player bots to generate dynamic, in-character chat responses using advanced natural language processing locally on your computer (or remotely hosted). Bots are enriched with personality traits, random chatter triggers, and context-aware replies that mimic the language and lore of World of Warcraft.

## Features

- **Ollama LLM Integration:**  
  Bots generate chat responses by querying an external Ollama API endpoint. This enables natural and contextually appropriate in-game dialogue.

- **Player Bot Personalities:**  
  When enabled, each bot is assigned a personality type (e.g., Gamer, Roleplayer, Trickster) that modifies its chat style. Personalities influence prompt generation and result in varied, immersive responses.

- **Context-Aware Prompt Generation:**  
  The module gathers extensive context about both the bot and the interacting player—including class, race, role, faction, guild, and more—to generate prompts for the LLM. A comprehensive WoW cheat sheet is appended to every prompt to ensure the LLM replies with accurate lore, terminology, and in-character language spanning Vanilla WoW, The Burning Crusade, and Wrath of the Lich King.

- **Random Chatter:**  
  Bots can periodically initiate random, environment-based chat when a real player is nearby. This feature adds an extra layer of immersion to the game world.

- **Chat Memory (Conversation History):**  
  Bots now have configurable short-term chat memory. Recent conversations between each player and bot are stored and included as context in every LLM prompt, giving responses better context and continuity.

  Bots now recall your recent interactions—responses will reflect the last several lines of chat with each player.

- **Blacklist for Playerbot Commands:**  
  A configurable blacklist prevents bots from responding to chat messages that start with common playerbot command prefixes, ensuring that administrative commands are not inadvertently processed. Additional commands can be appended via the configuration.

- **Asynchronous Response Handling:**  
  Chat responses are generated on separate threads to avoid blocking the main server loop, ensuring smooth server performance.

- **Live Configuration & Personality Reload:**  
  Reload the module’s config and personality packs in-game or from the server console, without restarting.

- **Event-Based Chatter:**  
  Player bots now comment on key in-game events such as quest completion, rare loot, deaths, PvP kills, leveling up, duels, learning spells, and achievements. Remarks are context-aware, immersive, and personality-driven, making the world feel much more alive.

- **Party-Only Bot Responses:**  
  When enabled, bots will only respond to real player messages and events when they are in the same non-raid party. This helps reduce chat spam while maintaining full bot-to-bot communication within parties for immersive group interactions.

- **Think Mode Support:**  
  Bots can leverage LLM models that have reasoning/think modes. Enable internal reasoning for models that support it by setting `OllamaChat.ThinkModeEnableForModule = 1` in **mod-ollama-chat.conf**. When enabled, the API request includes the `think` flag and the bot omits all `thinking` responses from its final reply.

- **Live Reload for Personalities and Settings:**  
  Instantly reload all mod-ollama-chat configuration and personality packs in-game using the `.ollama reload` command with a GM level account or use `ollama reload` from the server console. No server restart required—updates to `.conf` or personality packs (`.sql` files) are applied immediately.

## Installation

> [!IMPORTANT]
> **Cross-Platform Support**: This module now uses cpp-httplib (header-only) instead of curl, eliminating compilation issues on Windows and simplifying installation on all platforms.

1. **Prerequisites:**
   - Ensure you have liyunfan1223's AzerothCore (https://github.com/liyunfan1223/azerothcore-wotlk) installation with the Player Bots (https://github.com/liyunfan1223/mod-playerbots) module enabled.
   - The module depends on:
     - **fmtlib** (https://github.com/fmtlib/fmt) - For string formatting
     - **nlohmann/json** (https://github.com/nlohmann/json) - For JSON processing (**bundled with module** - no installation needed)
     - cpp-httplib (https://github.com/yhirose/cpp-httplib) - Header-only HTTP library (included, no installation needed)
     - Ollama LLM support – set up a local instance of the Ollama API server with the model of your choice. More details at https://ollama.com

2. **Install Dependencies:**

   ### Windows (vcpkg):
   ```bash
   vcpkg install fmt
   ```

   ### Ubuntu/Debian:
   ```bash
   sudo apt update
   sudo apt install libfmt-dev
   ```

   ### CentOS/RHEL/Fedora:
   ```bash
   sudo yum install fmt-devel  # or dnf install fmt-devel
   ```

   ### macOS (Homebrew):
   ```bash
   brew install fmt
   ```

   ### Arch Linux:
   ```bash
   sudo pacman -S fmt
   ```

3. **Clone the Module:**
   ```bash
   cd /path/to/azerothcore/modules
   git clone https://github.com/DustinHendrickson/mod-ollama-chat.git
   ```

4. **Recompile AzerothCore:**
   ```bash
   cd /path/to/azerothcore
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   ```

5. **Configuration:**
   Copy the default configuration file to your server configuration directory and change to match your setup (if not already done):
   ```bash
   cp /path/to/azerothcore/modules/mod-ollama-chat/mod-ollama-chat.conf.dist /path/to/azerothcore/etc/config/mod-ollama-chat.conf
   ```

6. **Restart the Server:**
   ```bash
   ./worldserver
   ```

## Setting up Ollama Server

This module requires a running Ollama server to function. Ollama allows you to run large language models locally on your machine.

### Installing Ollama

Download and install Ollama from [ollama.com](https://ollama.com). It supports Windows, macOS, and Linux.

- **Windows/macOS:** Download the installer from the website and run it.
- **Linux:** Follow the installation instructions for your distribution (e.g., `curl -fsSL https://ollama.com/install.sh | sh`).

### Starting the Ollama Server

Once installed, start the Ollama server:

```bash
ollama serve
```

This will start the server on `http://localhost:11434` by default.

### Running Ollama Across the Network

If you want to run the Ollama server on a different computer than your AzerothCore server, set the `OLLAMA_HOST` environment variable to `0.0.0.0` before starting the server:

```bash
export OLLAMA_HOST=0.0.0.0
ollama serve
```

This binds the server to all network interfaces, allowing connections from other machines on your network. Update the `OllamaChat.ApiEndpoint` in `mod-ollama-chat.conf` to use the IP address of the machine running Ollama (e.g., `http://192.168.1.100:11434`).

> [!WARNING]
> Exposing Ollama to the network may pose security risks. Ensure your firewall allows traffic on port 11434 only from trusted networks, and consider additional security measures if exposing to the internet.

### Pulling a Model

Before using the module, pull a model that the bots will use for generating responses. For example, to pull the Llama 3.2 1B model:

```bash
ollama pull llama3.2:1b
```

You can find available models at [ollama.com/library](https://ollama.com/library). Choose a model that fits your hardware capabilities.

### Connecting the Module

The module connects to the Ollama API via the configuration in `mod-ollama-chat.conf`. The default endpoint is `http://localhost:11434`. If your Ollama server is running on a different host or port, update the `OllamaChat.ApiEndpoint` setting.

### Checking if Ollama is Running

To verify that the Ollama server is running and accessible, you can test the API:

```bash
curl http://localhost:11434/api/tags
```

This should return a JSON response listing available models. If you get a connection error, ensure the server is started and the endpoint is correct.

## Configuration Options

> For a complete list of all available configuration options with comments and defaults, see `mod-ollama-chat.conf.dist` included in this repository.

## Text Commands

The module provides several in-game text commands for administrators (Game Masters) to manage and monitor the Ollama chat functionality. All commands require **SEC_ADMINISTRATOR** security level (GM level 3 or higher).

### `.ollama reload`
Reloads the module's configuration from `mod-ollama-chat.conf` without restarting the server. Also reloads personality packs and sentiment data.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama reload`
- **Console Equivalent:** `ollama reload`

### `.ollama sentiment view [bot_name] [player_name]`
Displays sentiment tracking data between bots and players.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:**
  - `.ollama sentiment view` - Shows all sentiment data
  - `.ollama sentiment view BotName` - Shows sentiment data for a specific bot
  - `.ollama sentiment view BotName PlayerName` - Shows sentiment between specific bot and player
- **Console Equivalent:** `ollama sentiment view [bot] [player]`

### `.ollama sentiment set <bot_name> <player_name> <value>`
Manually sets the sentiment value between a bot and player (0.0 to 1.0).
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama sentiment set BotName PlayerName 0.8`
- **Console Equivalent:** `ollama sentiment set <bot> <player> <value>`

### `.ollama sentiment reset [bot_name] [player_name]`
Resets sentiment data to default values.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:**
  - `.ollama sentiment reset` - Resets all sentiment data
  - `.ollama sentiment reset BotName` - Resets all sentiment data for a specific bot
  - `.ollama sentiment reset BotName PlayerName` - Resets sentiment between specific bot and player
- **Console Equivalent:** `ollama sentiment reset [bot] [player]`

### `.ollama personality get <bot_name>`
Displays the current personality assigned to a bot.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality get BotName`
- **Console Equivalent:** `ollama personality get <bot>`

### `.ollama personality set <bot_name> <personality>`
Manually assigns a personality to a bot.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality set BotName Gamer`
- **Console Equivalent:** `ollama personality set <bot> <personality>`

### `.ollama personality list`
Lists all available personalities and their descriptions.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality list`
- **Console Equivalent:** `ollama personality list`

> [!NOTE]
> All commands can also be executed from the server console by replacing the leading dot (.) with the command prefix used in your console (typically none or a custom prefix).

### `.ollama status`

Shows what the module currently thinks it is doing. Reports the endpoint and
model, whether think mode is supported and why, dispatcher queue depth and
worker count, delivery and drop counters, governor state (including *why*
replies were suppressed), and the last error.

This is the first thing to run when bots go quiet.

### `.ollama test <prompt>`

Sends one prompt straight to Ollama and writes the raw output and the
post-processed output side by side to the server log (`module.ollamachat`),
along with the round-trip time and whether think mode was used. Turns "the bots
aren't talking" into a one-command diagnosis.

## How It Works

1. **Chat Filtering and Triggering**  
   When a player (or bot) sends a chat message, the module checks the message's type, distance, and if it starts with any configured blacklist command prefix. If party restrictions are enabled, only bots in the same non-raid party as the real player can respond. Only eligible messages in range and not matching the blacklist will trigger a bot response.

2. **Bot Selection**  
   The system gathers all bots within the relevant distance, determines eligibility based on player/bot reply chance, and caps responses per message using `MaxBotsToPick` and related settings.

3. **Prompt Assembly**  
   For each reply, a prompt is assembled by combining configurable templates with live in-game context: bot/player class, race, gender, role/spec, faction, guild, level, zone, gold, group, environment info, personality, and if enabled, recent chat history between that player and the bot.

4. **LLM Request**  
   The prompt is sent to the Ollama API using the configured model and parameters. All LLM requests run asynchronously, ensuring no lag or blocking of the server.

5. **Response Routing**  
   Bot responses are routed back through the appropriate chat channel in game, whether it’s say, yell, party or general.

6. **Personality Management**  
   If RP personalities are enabled, each bot uses its assigned personality template. Personality definitions can be changed on the fly and reloaded live—no server restart required.

7. **Random & Event-Based Chatter**  
   In addition to responding to direct chat, bots will occasionally generate random environment-aware lines when real players are nearby, and will also react to key in-game events (e.g., PvP/PvE kills, loot, deaths, quests, duels, level-ups, achievements, using objects) using context-specific templates and personalities.

8. **Live Reloading**  
   You can hot-reload the module config and personality packs in-game using the `.ollama reload` GM command or from the server console. All changes take effect immediately without requiring a restart.

9. **Fully Configurable**  
   All settings—reply logic, distances, frequencies, blacklist, prompt templates, chat history, personalities, random/event chatter, LLM params, and more—are controlled via `mod-ollama-chat.conf` and can be adjusted and reloaded live at any time.

## Personality Packs

`mod-ollama-chat` supports Personality Packs, which are collections of personality templates that define how bots roleplay and interact in-game.

- To use a Personality Pack, download or create a `.sql` file named in the format `YYYY_MM_DD_personality_pack_NAME.sql`.

- Place the `.sql` file in `modules/mod-ollama-chat/data/sql/characters/updates/`.

- The module will automatically detect and apply any new Personality Packs when the server starts or updates—no manual SQL import required.

Want to create your own pack or download packs made by the community?  

Visit the [Personality Packs Discussion Board](https://github.com/DustinHendrickson/mod-ollama-chat/discussions)

## Debugging

For detailed logs of bot responses, prompt generation, and LLM interactions, enable debug mode via your server logs or module-specific settings.



## Conversation Control

Bot chat can run away in several different ways, so there are four independent
brakes. All are configurable; see the `CONVERSATION GOVERNOR` section of the
config file.

| Brake | What it stops |
|---|---|
| **Chain depth + decay** | A bot replying to a bot replying to a bot. Each hop also multiplies the reply chance down, so chains lose energy before hitting the hard ceiling. |
| **The audience rule** | Bots holding conversations with nobody listening. Bots may only reply to *other bots* while a real player has spoken in that channel recently. This does most of the work. |
| **Cooldowns and rate limits** | One bot, or one crowd, dominating a channel. Per-bot, per-channel, and server-wide. The global limit also caps your LLM spend. |
| **Repetition scoring** | The same line twice, and the same *opening phrase* twice. Candidate replies are scored against the bot's own recent lines and the channel's recent traffic. |

If bots are looping, the setting to reach for first is
`OllamaChat.BotConversation.RequireRecentHuman`.

## What Bots Talk About

Topics are chosen from weighted categories rather than uniformly, and each bot
suppresses whatever it used in its last few picks.

| Category | Default weight | Examples |
|---|---|---|
| **People** | 30 | Nearby players by name, class and what they are doing; group members; guildmates online; something the bot just watched happen |
| **World** | 30 | The nearest *interesting* creature (elite, rare, or a real level threat — never a critter); named NPCs by role; landmarks; corpses; time of day |
| **Activity** | 25 | Current quest objectives; danger assessment; group needs (someone low, someone out of mana) |
| **Self** | 15 | Spells, equipped items, bag space — the original topics, kept but demoted |

**Witnessed-event memory.** Bots keep a short memory of what they saw happen
near them — kills, deaths, level-ups, loot. This is what lets a bot comment on
the fight you were both just in rather than reciting a fact about itself.

Note that `OllamaChat.Snapshot.IncludeSpells` now defaults to **0**. Listing
every off-cooldown spell a bot knew put dozens of lines of the most quotable
text in the prompt, which is why bots talked about their spellbook so much.

## Memory and Relationships

Conversation history is a sliding window. Once a line falls out of it the bot
has no idea it ever happened, which is why bots otherwise feel like they meet
you fresh every session. Two mechanisms give them continuity, both bounded so
the prompt never grows without limit however long a character has been alive.

**Memory.** History accumulates until it crosses a token budget. At that point
the model condenses it into a handful of short narrator-style notes, each
scored 1-10 for importance, and the raw history is cleared. At prompt-build
time the most important notes are selected within a separate, smaller budget.
A character gradually accumulates what mattered and forgets the small talk.

**Relationships.** When a name comes up often enough in a bot's history, the
model is asked to write -- or revise -- a sentence on how that bot feels about
that person. That sentence goes into future prompts, so a character's attitude
toward you persists and evolves rather than resetting. Whoever the bot is
currently talking to is always listed first, so their own relationship never
gets squeezed out by the budget.

Both work in normal mode and in roleplay mode. They complement the numeric
sentiment score rather than replacing it: sentiment is how warm the bot feels,
these are *why*.

Condensation and relationship writing are themselves LLM calls, so they run on
the dispatcher's workers with a stricter queue allowance than replies get --
background upkeep can never crowd out live conversation.

Requires `data/sql/characters/base/2026_08_29_memory_relationships.sql`.
Tune it under the `LONG-TERM MEMORY AND RELATIONSHIPS` section of the config.

| Setting | Default | Effect |
|---|---|---|
| `Memory.HistoryTokenLimit` | 1500 | How much history accumulates before it is distilled |
| `Memory.PromptTokenBudget` | 400 | Hard stop on how much of the prompt memories may use |
| `Memory.MaxPerBot` | 40 | Notes retained per character; least important dropped first |
| `Relationship.MentionThreshold` | 8 | How often a name must come up before an opinion is written |
| `Relationship.MaxPerPrompt` | 3 | Relationships included per prompt |

## Roleplay Mode

Off by default. Turn on with `OllamaChat.Roleplay.Enable`.

Gives each race a speech register and cultural touchstones, and each class a
worldview — what that character *notices*. A Tauren speaks slowly of the
Earthmother and the balance; a Forsaken is dry and calls the living "breathers";
a priest sees wounds, a hunter reads tracks, a rogue counts exits.

`OllamaChat.Roleplay.Strictness` controls how far it goes:

- **0** — Flavour only. Voices colour the prompt, nothing else changes.
- **1** — In character. Out-of-world vocabulary (`dps`, `nerf`, `patch`, …) is
  rejected rather than spoken, and faction attitude is applied.
- **2** — Hard in character. In-world chatter lists replace the shipped
  out-of-character ones, injuries and distances are described rather than
  quoted as figures, and think mode is used where the model supports it.

`OllamaChat.Roleplay.CrossFactionGibberish` (on by default in roleplay mode)
stops bots answering across factions in say/yell — the client renders those as
gibberish anyway, so a fluent reply is the most immersion-breaking thing the
module can do.

Race and class voices can be overridden per server in the
`mod_ollama_chat_voice` table without a rebuild.

## Body Language

Replies are accompanied by movement so they read as conversation rather than as
a log line.

- **Facing** — the bot turns toward whoever it is answering, just before the
  line lands. Skipped while moving, casting, in combat, in flight, on a
  transport, or teleporting.
- **Gestures** — the model may end a reply with `[emote:wave]`, `[emote:nod]`
  and similar. The tag is parsed out and stripped before the line is spoken.
  `*waves*` and a bare `/wave` are also recognised, because models emit those
  unprompted. Emotes are played through the same code path the client uses, so
  nearby players see both the animation and the "Bot waves at You." social text,
  with the correct per-race and per-gender variant.
- **Emote reactions** — bots react when a player emotes at them, either by
  mirroring (wave gets a wave), countering (flex gets a laugh), or answering in
  words. The first two cost no LLM call.

## Think Mode

`OllamaChat.ThinkMode` takes `auto` (default), `on`, or `off`.

Under `auto` the module asks Ollama what the configured model can actually do
and only ever sends `think` to a model that reports the capability — so a model
that cannot think is never asked to. It then spends reasoning only where it
changes the answer: off for short chat lines, on for sentiment analysis and
strict roleplay replies.

If a live request is ever rejected for asking to think, the module remembers
that, logs it once, and retries without it. A latency guard
(`OllamaChat.ThinkMaxLatencyMs`) backs think mode off for the session if it
proves too slow for chat. You do not need to restart after swapping models —
`.ollama reload` re-probes.

## Threading Model

Worker threads do HTTP and string work only. Every read or write of a `Player`,
`Channel`, `Guild`, `Group` or `Map` happens on the world thread.

Requests are built on the world thread, handed to a bounded worker pool
(`OllamaChat.WorkerThreads`), and delivered back on the world thread by a
completion queue drained each tick. Queue depth is capped
(`OllamaChat.MaxQueueDepth`) so a slow Ollama sheds load instead of building a
backlog of stale replies.

## License

This module is released under the GNU GPL v3 license, consistent with AzerothCore's licensing.

## Contribution

Developed by Dustin Hendrickson

Pull requests, bug reports, and feature suggestions are welcome. Please adhere to AzerothCore's coding standards and guidelines when submitting contributions.
