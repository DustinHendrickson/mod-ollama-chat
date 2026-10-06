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

- **Other LLM Providers:**  
  Not limited to Ollama. Set `OllamaChat.Provider` to `openai` for anything that speaks the OpenAI Chat Completions format (OpenAI, OpenRouter, Groq, Mistral, DeepSeek, xAI, Gemini, LM Studio, vLLM, llama.cpp) or to `anthropic` for Claude. See [Using Other LLM Providers](#using-other-llm-providers-openai-openrouter-claude).

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

- **Autopilot (experimental):**  
  The LLM runs selected bots the way a player runs a character. It writes each bot's identity (playstyle, outlook, what it loves and what bores it), sets its goals, and switches playerbots strategies on and off to pursue them, based on everything that has happened to the bot. Playerbots still does the fighting and walking. See [Autopilot](#autopilot-llm-driven-bots).

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
   cp /path/to/azerothcore/modules/mod-ollama-chat/conf/mod_ollama_chat.conf.dist /path/to/azerothcore/env/dist/etc/modules/mod_ollama_chat.conf
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

This binds the server to all network interfaces, allowing connections from other machines on your network. Update the `OllamaChat.Url` in `mod-ollama-chat.conf` to use the IP address of the machine running Ollama (e.g., `http://192.168.1.100:11434`).

> [!WARNING]
> Exposing Ollama to the network may pose security risks. Ensure your firewall allows traffic on port 11434 only from trusted networks, and consider additional security measures if exposing to the internet.

### Pulling a Model

Before using the module, pull a model that the bots will use for generating responses. For example, to pull the Llama 3.2 1B model:

```bash
ollama pull llama3.2:1b
```

You can find available models at [ollama.com/library](https://ollama.com/library). Choose a model that fits your hardware capabilities.

### Connecting the Module

The module connects to the Ollama API via the configuration in `mod-ollama-chat.conf`. The default endpoint is `http://localhost:11434`. If your Ollama server is running on a different host or port, update the `OllamaChat.Url` setting.

### Checking if Ollama is Running

To verify that the Ollama server is running and accessible, you can test the API:

```bash
curl http://localhost:11434/api/tags
```

This should return a JSON response listing available models. If you get a connection error, ensure the server is started and the endpoint is correct.

## Using Other LLM Providers (OpenAI, OpenRouter, Claude)

Ollama is the default, but it is not required. The only Ollama-specific part
of the module is how a single request is encoded, so any service that speaks
one of two common API formats can be used instead. Four settings in
`mod-ollama-chat.conf` control this:

| Setting | Meaning |
|---|---|
| `OllamaChat.Provider` | `ollama` (default), `openai`, or `anthropic` |
| `OllamaChat.Url` | The full URL of the provider's generation endpoint |
| `OllamaChat.Model` | The model name as the provider publishes it |
| `OllamaChat.ApiKey` | The provider's API key. Leave empty for local servers that need none |
| `OllamaChat.ApiKeyHeader` | Optional. Only for services with their own header name, such as Azure OpenAI (`api-key`) |

`openai` means the **OpenAI Chat Completions format**, which is the de facto
standard and is spoken by far more than OpenAI: OpenRouter, Groq, Together,
Mistral, DeepSeek, xAI, Google Gemini (through its OpenAI-compatible
endpoint), LM Studio, vLLM, llama.cpp server, and Ollama's own
`/v1/chat/completions` route all accept it. `anthropic` is the native Claude
Messages API. Claude is also reachable through OpenRouter using `openai`.

### Examples

**OpenAI**

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = https://api.openai.com/v1/chat/completions
OllamaChat.Model    = gpt-4o-mini
OllamaChat.ApiKey   = sk-...
```

**OpenRouter** (one key, access to many models from many vendors)

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = https://openrouter.ai/api/v1/chat/completions
OllamaChat.Model    = openai/gpt-4o-mini
OllamaChat.ApiKey   = sk-or-...
```

**Anthropic (Claude)**

```ini
OllamaChat.Provider = anthropic
OllamaChat.Url      = https://api.anthropic.com/v1/messages
OllamaChat.Model    = claude-haiku-4-5-20251001
OllamaChat.ApiKey   = sk-ant-...
```

**LM Studio** (local, no key needed)

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = http://localhost:1234/v1/chat/completions
OllamaChat.Model    = <the model id LM Studio shows in its server tab>
```

### Steps

1. Edit the four settings above in `mod-ollama-chat.conf`.
2. Restart `worldserver`, or run `.ollama reload` in game or `ollama reload`
   on the console.
3. Run `.ollama status`. The first line now shows the provider and whether a
   key is set (the key itself is never printed).
4. Run `.ollama test hello` to send one request and see the reply, or the
   provider's error message if something is wrong (bad key, wrong model name,
   wrong URL).

### What to know

- **HTTPS requires an OpenSSL build.** The CMake output says
  `[mod-ollama-chat] OpenSSL found - HTTPS support enabled` when it is, and
  `.ollama status` shows `HTTPS: available`. Without it, an `https://` URL is
  reported as an error at startup and every request fails. On Windows install
  OpenSSL (for example `vcpkg install openssl` or the Win64 OpenSSL installer)
  and re-run CMake; on Linux install `libssl-dev` or `openssl-devel`.
- **Certificates are verified when a key is set.** By default the module skips
  TLS certificate checks (so self-signed reverse proxies in front of a local
  Ollama keep working) but turns them on whenever `OllamaChat.ApiKey` is
  non-empty, since a key should never travel over an unverified connection.
  `OllamaChat.VerifyCertificates` overrides this either way.
- **API keys.** Every hosted provider needs one; put it in `OllamaChat.ApiKey`.
  The module sends it as a bearer token for OpenAI format and as
  `x-api-key` for Anthropic; `OllamaChat.ApiKeyHeader` overrides the header
  name for services like Azure OpenAI. A rejected key (HTTP 401/403) is
  reported once in the log with what to check.
- **Cost and rate limits.** Random chatter and event chatter across many bots
  adds up fast on a metered API, and hosted providers answer bursts with
  HTTP 429. The module waits and retries a bounded number of times
  (`OllamaChat.RateLimitRetries`, honouring the provider's `Retry-After`,
  capped by `OllamaChat.RateLimitMaxWaitSeconds`) and logs the first
  occurrence. Start with `OllamaChat.EnableRandomChatter = 0` and a low
  `OllamaChat.MaxConcurrentQueries`, and watch the provider's usage page before
  opening it up.
- **Settings that do not apply are not sent.** `NumCtx`, `NumThreads`,
  `RepeatPenalty`, `MinP` and `TopK` have no equivalent in the OpenAI format;
  Anthropic additionally has no `TopP`, `Seed`, or penalties. `NumPredict`
  works everywhere (Anthropic requires a cap, so `0` becomes 1024 there).
- **Think mode.** There is no capability probe for these providers, since
  that would be a billed request. Under `ThinkMode = auto` reasoning is never
  requested. Under `on` it is requested on every call (`reasoning_effort` for
  OpenAI format, an extended-thinking budget for Anthropic) and switched off
  for the session if the provider rejects it.
- **Self-healing parameters.** Newer OpenAI models reject `max_tokens` in
  favour of `max_completion_tokens`, and OpenAI reasoning models reject
  explicit sampling values. Both are detected from the first rejection, logged
  once, and the request is retried immediately, so no reply is lost.
- **Keep the key private.** Treat `mod-ollama-chat.conf` like
  `worldserver.conf`: readable only by the account running the server, and
  never committed to a repository.

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

### `.ollama autopilot ...`
Controls the autopilot feature (the LLM as the bot's master): `status`, `preview`, `on`, `off`, `rules`, `history`, `identity`, `goal`, `replan`. See [Autopilot commands](#commands).

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

### The Ollama Monitor addon

`addon/OllamaMonitor` is a client addon for WoW 3.3.5a that shows what the
bots are doing, live, without reading logs.

**Install:** copy the `addon/OllamaMonitor` folder into your client's
`Interface/AddOns` folder. Log in on an account at
`OllamaChat.Monitor.MinSecurity` (administrator by default), then type `/om`
or click the spyglass button on the minimap (left-click shows or hides the
window, right-click switches to log only, drag to move it).

**The window:**
- **Left:** every online autopilot bot, with its class, level and what it is
  doing (fighting, travelling, waiting on the model, idle, ...). The box at
  the top filters by name, zone, state or plan. Hover over a bot for its
  current plan; click it to open its pages.
- **Right**, one page at a time, refreshed automatically:
  - **Overview:** where it is, health, combat and attackers, its movement
    type, playerbots' engine and live strategies, the model's identity, plan,
    goal, reason and last orders, and the watchdogs (alerts, deaths,
    teleport holds).
  - **Travel:** the errand, every leg of the trip with the current one marked,
    and the route builder's state: next node, road anchors, corridor, stalls,
    slope step-offs, navmesh queries spent.
  - **Planner:** the last prompt sent to the model for this bot, and its raw
    reply or error.
  - **Chat:** personality, pacing (cooldowns, conversations), queued replies,
    the bot's recent replies with what each was answering and what became of
    it (said, or why it was dropped), the conversation history kept for
    prompts, and the last chat prompt and raw reply.
  - **Mind:** memories, relationships, sentiment toward players, recent topics
    and witnessed events.
  - **Events:** orders and their results, recent decisions, the diary.
- **Buttons:**
  - **Follow camera** puts your camera on the bot, the way Mind Vision
    does. Your own character stays where it is. When the bot changes map,
    you are teleported along. A dungeon can't be followed: the camera
    waits until the bot comes out.
  - **Teleport to** moves you to the bot.
  - **Replan now** and **Status to chat** run the matching
    `.ollama autopilot` command.
  - **Turn off / Turn on.** While the selected bot is on autopilot, the
    button reads Turn off. After you confirm, it runs
    `.ollama autopilot off <bot>`: the bot's strategies go back to what it
    had before autopilot, and it leaves the list. The button then reads Turn
    on, which runs `.ollama autopilot on` for that bot. With nothing
    selected, Turn on uses your current target.
  - **Plain text** turns off colours, so text copies cleanly. Every page
    is selectable, and Ctrl+C copies it.
- **Log only:** the small button beside the close X shrinks the window to
  the bot's name and the live log, see-through, so most of the screen
  stays free while the camera follows. Drag the corner to resize it, and
  click the button again for the full window. `/om mini` does the same, and
  the mode and size are remembered.
- **Whisper:** talks to the bot through the normal chat system. Its answer
  shows up on the Chat page.

**Slash commands:**
- `/om` opens or closes the window.
- `/om stop` releases the camera.
- `/om mini` switches between the full window and log only.
- `/om pause` and `/om resume` stop and restart the auto refresh.



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

## Passer-by Buffs

In the old world a stranger running past would often throw you a Power Word:
Fortitude or a Mark of the Wild. Bots do the same now. Out of combat, a bot
buffs friendly players near it (and other bots, unless
`OllamaChat.Buff.BuffBots` is 0) with the buffs its class knows, and
sometimes waves. It won't buff someone it dislikes: anyone whose sentiment
is below `OllamaChat.Buff.MinSentiment`.

Buffs are taken from each bot's own spellbook. Only instant, long-lasting,
single-target buffs for allies are used: Fortitude, Divine Spirit, Shadow
Protection, Arcane Intellect, Mark of the Wild, Thorns and the paladin
blessings. Paladins give one blessing per person: Kings if they know it,
otherwise Wisdom for mana users and Might for everyone else.

Bots never buff over a buff someone already has, and they don't stop to
buff while mounted, low on mana, eating or casting. A PvP-flagged player
only gets buffed by a bot that is flagged too. All of these settings live
under `OllamaChat.Buff.*` in the conf.

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

With a non-Ollama provider (`OllamaChat.Provider = openai` or `anthropic`)
nothing is probed, because the only way to ask would be a billed request.
`auto` then never requests reasoning; `on` requests it on every call and backs
off for the session if the provider rejects it.

## Autopilot (LLM-driven bots)

> [!WARNING]
> Experimental and off by default. Try it on a few bots first.

Autopilot makes the LLM the **controller** of selected bots. It decides who
each character is, what it wants, and what it does about it, and then gives
the orders a player would whisper to their own bot:

```
nc +grind,-quest          quest 783 reward 2   goto trainer
goto zone Tanaris         talents spec arms    equip upgrade
goto profession mining    goto vendor          b vendor
co +flee,+potions         nc +lfg              goto repair
```

Playerbots' own overhead controllers (`new rpg`, the older `rpg` wanderer and
the `travel` planner) are switched off while a bot is on autopilot, so
nothing else picks where the bot goes or what it sets out to do. Playerbots'
AI still fights, loots, gathers and casts. Without orders, an autopilot bot
stands where it is.

### A bot's evening

```
[plan]   "off to Tanaris to see the desert" for 60m [goto zone Tanaris]
[order]  goto zone Tanaris -> on the way to Tanaris (another continent)
[travel] boarded The Maiden's Fancy
[travel] arrived in Kalimdor by The Maiden's Fancy
[travel] took a flight to Gadgetzan, Tanaris
[travel] landed at Gadgetzan, Tanaris
[errand] arrived in Tanaris
[plan]   "first, a proper bed" for 20m [goto inn]
```

Every line is in `.ollama autopilot history <bot>`.

### What the LLM decides

- **Identity.** On a bot's first plan the LLM writes who it is: its playstyle,
  its outlook (*in-character*, casual *player* or *metagamer*), and what it
  loves, what bores it and what tempts it. It can revise this later.
- **Goals.** Reach a level, raise a profession, earn gold, visit a zone,
  finish quests, run dungeons, or anything else in its own words. Progress is
  measured from the bot, not taken on the LLM's word.
- **Orders.** Up to 8 per plan, run in order. Any playerbots command works
  unless the operator denies it. Everything after a `goto` or `quest` waits
  until the bot gets there ("goto vendor, b vendor" buys at the vendor);
  strategy changes apply at once. A new plan with a trip replaces orders
  still waiting.
- **A player's choices.** Which quest reward to take, which professions to
  learn, which talent spec to follow, what to buy and sell. Code never picks
  these for an enrolled bot.
- **When to look again.** Each plan says how long it holds. When a trip ends,
  the LLM is asked again within a minute (`QuickReplanSeconds`), so the bot
  doesn't stand idle.

### What the LLM sees

- **The character:** level, class, zone, gold, gear durability and free
  bag slots.
- **Quests:** its quest log with ids. A quest that's ready to turn in says
  who takes it and where: the zone and distance, or "on another continent".
  Under it, each reward choice is listed with its kind, armor or damage,
  stats, and whether the bot can use it. A quest that isn't for the bot's
  class or race is marked, so the LLM abandons it instead of crossing the
  world for it.
- **Professions and talents:** its professions with skill levels, and how
  many primary profession slots are free. From level 10, its unspent talent
  points and its class's talent specs.
- **Surroundings:** the nearest services (trainer, repair vendor, inn and so
  on) and zones that suit its level.
- **What's around it, worth acting on:** bodies to loot or skin, herb and ore
  nodes (and whether it has the skill), chests, fishing pools and crafting
  stations nearby. A player notices these and is tempted by them, and so is
  the LLM.
- **What it could craft:** recipes it knows that it has the materials for,
  how many, whether they still raise the skill, and which need a forge or
  anvil.
- **What it's doing:** its live strategies, what it's doing right now
  (walking, flying, waiting for a boat), and the orders still queued.
- **Feedback:** what each of its last orders actually did, so it can try
  something else when one fails.
- **History:** its goal and progress, rewards in the last hour, recent
  events, and temptations (a dungeon just unlocked, an epic drop, a capped
  profession).

The order list it's given is in `mod-ollama-chat_autopilot_commands.cpp`
(`OllamaChat.Autopilot.CommandReference` replaces it). The monitor addon's
Planner tab shows the full prompt it was sent.

### What autopilot controls, and what it doesn't

**The LLM controls:**

| Area | Orders | Where |
|---|---|---|
| Behaviours | `nc +x,-y` / `co +x,-y`: any playerbots strategy except the overhead controllers (grind, quest, gather, loot, lfg, bg, pvp, flee, potions, aoe, ...) | Combat ones everywhere; the rest only while the bot is on its own |
| Quests | `quest <id>` goes to where the objective is (a creature it still needs, or the quest's map marker). On arrival the bot stays and works the objective, the nearest thing it still needs first: creatures to kill, objects the quest says to use (it uses them as the client does), and the creatures and objects that give the quest's items. Crates, chests and plants holding a quest item are opened on the spot: herbs and ore with the gathering skill, locked ones with a key or the right opening spell (playerbots' own way of opening each lock), unlocked ones by using them. The bot then takes everything inside. An object that won't open after three tries is skipped. When none are in sight, it moves on to where more are; when no more are known, it explores the area (four legs of about 120 yards) before giving up, until the objective is done or `QuestHuntMinutes` (20) pass. Once the quest is complete, it goes to whoever takes it in and turns it in. The prompt lists a finished quest's reward choices (kind, stats, usable or not), and `quest <id> reward <n>` takes the one the character wants. `abandon <id>` drops a quest, as the client's Abandon button does. A quest that isn't for the bot's class or race is refused | On its own |
| Errands | `goto repair / vendor / trainer / inn / flightmaster` goes there and uses it: repairs and sells junk, learns every affordable class spell, sets the inn as home, or learns the flight point. `goto profession <name>` goes to that profession's trainer and learns it, its next rank and its recipes; the prompt shows the free profession slots, and which professions to take is the character's choice. `goto bank / auction / mailbox` goes there; at a bank, `bank <item>` stores items, and at a mailbox the bot collects everything waiting | On its own |
| People | `goto <name>` walks up to an NPC by name ("goto Deputy Willem") and talks to them, so the quests they offer are taken and finished ones handed in | On its own |
| Travel | `goto zone <name>`, anywhere in the world: walking, flight masters, boats, zeppelins and portals (the Dark Portal, city portals) as needed | On its own |
| Hunting | `goto hunt` goes to the nearest group of monsters of the bot's level. When none is known nearby, the bot explores instead: about 120 yards at a time to walkable ground, turning each leg so it spirals outwards, looking after each leg for live monsters of its level and heading for the first it finds (six legs, then it reports that it found nothing); `nc +grind` fights what is around it | On its own |
| Upkeep and money | `talents spec <name>` (one of the class's talent specs, listed in the prompt with unspent points), `equip upgrade` (best gear in the bags), `e` / `use <item>`, `open items`; at a vendor `s gray` / `s vendor` / `s <item>`, `b vendor` (buys what they can use, with their own gold), `repair`. Orders after a `goto` wait until the bot arrives | On its own |
| Auction house and mail | At an auctioneer: `ah look <item>` (the cheapest offers), `ah sell <item> [price each]` (one stack; by default just under the cheapest offer), `ah buy <item> [max price each]` (the cheapest it can afford). These go through the client's own auction handlers, so the deposit and cut are charged. Purchases, sale money and expired auctions come by mail, and `goto mailbox` collects them | On its own |
| Professions and the world | `nc +gather` picks herbs and mines ore as it passes, with its own skill. `nc +loot` loots bodies, and skins them with Skinning and a skinning knife. `nc +master fishing` fishes at water nearby. `open <object>` walks to a node, chest or object and opens or uses it. `craft <recipe> [count\|all]` makes something from its own materials. It first checks for the tool the recipe needs (a blacksmith hammer, a mining pick) and a station within reach, and walks to the nearest forge, anvil or fire if needed. A craft counts once the item is in its bags. `disenchant <item>` | On its own |
| Goals, identity, timing | Measurable aims, who the character is, how long a plan holds | Always |

### Earned, not handed out

With `OllamaChat.Autopilot.NoHandouts` (on by default), an enrolled bot has
only what it earned: what it loots, buys, is rewarded and trains for, like
a player. For enrolled bots only, playerbots' handouts are switched off:

- **Level-ups:** no free talents, trainer spells, consumables or gear, and
  no level-up teleport.
- **Re-rolls and refreshes:** none. Those empty the bags, set the money and
  hand out supplies.
- **Dungeon finder:** playerbots' accept refreshes a bot that queued alone.
  Autopilot accepts the group itself instead.
- **Death:** no free repair when releasing and no sickness-free spirit
  healer. The bot runs back to its body. If it can't make it in time, it
  takes the spirit healer's resurrection like a player, sickness and
  durability loss included.
- **Flights:** no taxi cheat. The bot flies only between flight points it
  has discovered.
- **Orders:** `maintenance`, `autogear` and `bis` are refused.

Enrolled random bots also stay logged in across restarts
(`OllamaChat.Autopilot.KeepOnline`). They are also kept out of playerbots'
level-bracket re-rolls (`AiPlayerbot.LevelBrackets.*`). Those skip any bot
on a friend list (with `LevelBrackets.IgnoreFriendListed`, on by default),
so autopilot adds a row for each enrolled bot to `character_social`. The row is owned by guid 0, which
isn't a character, so it shows on nobody's friend list. It carries the note
`ollama autopilot`, is rebuilt at startup and is removed when the bot leaves
autopilot.

**Running with no real players online** (a selfbot, or an empty realm):
autopilot itself doesn't need anyone nearby, but playerbots idles a bot with
no real player near for most of the time (`AiPlayerbot.BotActiveAlone`, 10%
by default). An idle bot runs only urgent actions, so it doesn't grind, loot
or buff. A selfbot counts as a real player, but only for bots in its zone or
within `BotActiveAloneForceWhenInRadius`. To keep every bot awake, set
`AiPlayerbot.BotActiveAlone = 100` and `AiPlayerbot.botActiveAloneSmartScale = 0`
in `playerbots.conf` (heaviest load), or the lighter
`AiPlayerbot.BotActiveAloneForceWhenInMap = 1` (bots on the same continent as
a real player or selfbot). The monitor's Overview shows each bot's
"playerbots activity".

Some things this module can't change per bot:

- **Free eating and drinking:** playerbots' global `AiPlayerbot.BotCheats`
  `food` cheat. Remove `food` there if you want bots to buy their food.
- **Reset at max level:** playerbots' `AiPlayerbot.ResetBotLevel.*` (off by
  default) puts a random bot back to a low level at the cap. It doesn't
  check friend lists. If you turn it on, add enrolled bots to
  `AiPlayerbot.ResetBotLevel.ExcludeNames`.

**Autopilot doesn't control:**

- **Fighting, targeting, casting and rotations.** Playerbots' AI does all of
  this, as it always has. A bot attacked on the way fights back, then carries
  on.
- **Orders on the deny-list.** By default that means logout, resets,
  destroying items, teleports, summons, mail, cheats, debug, raw `do`
  actions, leaving the group, releasing the spirit, guild management and
  playerbots' `rpg` commands (`Autopilot.DeniedCommands`).
- **Bots in a human's group.** The player leads, and only the LLM's combat
  orders (`co ...`) are carried out. With `WithRealPlayer = 0`, autopilot
  doesn't touch them at all.
- **Bots in a dungeon or battleground, or following a bot group's leader.**
  Only combat orders are carried out. On entry, the bot's out-of-combat
  strategies go back to what it had before autopilot, and any trip under
  way ends. When it leaves, the LLM's strategies return and it is asked what
  to do next.
- **Chat.** Chat replies don't mention the bot's plan yet. Autopilot and chat
  run side by side.
- **Bots it can't plan for.** With nobody nearby (the default reach), a bot
  keeps its last orders. A newly enrolled bot stands idle until its first
  plan arrives.

**Getting around.** Travel is autopilot's own; it doesn't use playerbots'
NewRpg.

- **Walking.** Anything further than 60 yards follows a route built on the
  server's navmesh, the way mod-city-siege routes its armies. Long walks
  follow playerbots' own road network (its travel nodes) so bots take roads
  and passes. A bot only ever moves along a real navmesh path, never in a
  straight line through walls or up cliffs. The bot is
  handed one node (about 28 yards) at a time, and the route is built a few
  hundred yards ahead as it walks. A bot standing on a slope too steep to
  path from steps a few yards onto walkable ground first. A stuck bot
  reroutes twice before the trip fails. Without mmaps, it walks straight at the destination.
- **Staying on the road.** While the bot is on a trip, its `grind` strategy
  doesn't pull monsters in reach (neutral beasts included) or wander off when
  there's nothing to fight. The trip is the order. Whatever attacks it is
  still fought, and grind picks up again on arrival.
- **Another way.** If a boat, portal or trigger doesn't work (a portal that
  does nothing, a dock it can't reach), the bot tries another crossing
  rather than giving up the trip.
- **Looting kills.** After a fight, an autopilot bot loots the bodies it
  may loot (within 25 yards) before it moves on, the way the client does:
  it opens each body, takes the coin and every item it can carry, and
  closes it. Quest items drop this way, so a quest hunt collects them as it
  goes. Skinning is still playerbots' `loot` strategy.
- **Casting on the way.** The walk waits while the bot casts (a pet summon,
  a buff) and stops for two seconds every thirty, as a player would, because
  playerbots never starts a spell with a cast time while the bot moves. A
  bot attacked on the way stops at once and fights.
- **Flights.** On the same continent, a flight is taken when it clearly
  saves distance and the bot can pay for it. Otherwise the bot walks. As for
  a player, only flight points the bot has discovered are used;
  `goto flightmaster` discovers the one it walks to. Bots pick up
  flight points as they pass flight masters, so they fly more as they
  travel.
- **Boats and zeppelins.** These are found at startup from the server's
  transports. A dock that belongs to the other faction (its guards and dock
  master) is never used, so an Alliance bot never goes to a Horde zeppelin
  tower; neutral ports such as Booty Bay are open to both. The bot waits at
  the dock, walks on when the ship docks, stays on deck, and steps off at the
  other end. Bots at a dock or aboard are checked every sweep, so they don't
  miss the minute a ship is in.
- **Portals.** The Dark Portal both ways (the bot walks into it, as a player
  does), and the city portals in Shattrath and Dalaran, the Silvermoon orb
  and the like (the bot uses them). A portal belonging to the other faction
  is never used.
- **Chained.** Crossings are chained as needed, for example Kalimdor by boat
  to the Eastern Kingdoms, then through the Dark Portal to Outland.

**No teleporting** (`NoTeleport`, on by default). Playerbots normally moves
random bots by teleport: every hour or so to a spot for their level, and
after a death instead of a corpse run. For autopilot bots:

- The periodic teleport is pushed back every hour.
- A dead bot releases, walks its ghost back to its body along a navmesh
  route, and reclaims it once the game's resurrection timer allows. After
  `CorpseRunMinutes` (10) it takes the spirit healer's resurrection, with
  resurrection sickness and durability loss, as a player would.

`NoRandomize` also holds playerbots' periodic re-roll, which would otherwise
empty the bot's bags, re-gear it and, at level 1–2 or the level cap, give
it a new level somewhere else. Both holds are set at startup, before any bot
logs in. Playerbots' level-up teleport (`AiPlayerbot.AutoTeleportForLevel`)
is dropped for enrolled bots by `NoHandouts`; with `NoHandouts` off, turn it
off in `playerbots.conf` for full coverage.

**Handing back.** Before autopilot changes anything, the bot's strategies are
recorded and saved in the database. When the bot is turned off or autopilot
is disabled, the bot goes back to exactly that, including the overhead
controllers.

### Quick start

1. Apply `data/sql/characters/base/2026_10_05_autopilot.sql` to your
   characters database. If you applied an earlier version, the table is
   migrated at startup.
2. In `mod_ollama_chat.conf`:
   ```ini
   OllamaChat.EnableChatBotSnapshotTemplate = 1   # required
   OllamaChat.Autopilot.Enable = 1
   OllamaChat.Autopilot.Select.RandomBotPercent = 5
   OllamaChat.Autopilot.Debug = 1                 # log every plan and order
   ```
3. Restart, or run `.ollama reload`. Stand near some bots: bots near real
   players are planned for first.
4. Run `.ollama autopilot status <bot>` to see the bot's identity, plan,
   goal, the LLM's strategies, what its AI is doing, and what each order did.
   Run `.ollama autopilot history <bot>` for the full diary.

### Choosing which bots

| Setting | Selects |
|---|---|
| `Select.RandomBotPercent` | That share of random bots. The selection is stable, so raising it only adds bots |
| `Select.RealPlayerGuilds` | Every bot in a guild that has a human member, online or not |
| `Select.Guilds` / `Select.Accounts` | Bots in the listed guild or account ids |
| `Select.Include` / `Select.Exclude` | Bots by name. Exclude always wins |
| `Select.AltBots` | Alt and addclass bots (off by default) |
| `Select.MinLevel` / `MaxLevel` | Level band for the rules above |
| `MaxEnrolled` | Cap on rule-selected bots. Deleted characters free their place |
| `AllowMasterEnroll` | Lets a bot's master whisper `nc +autopilot` |

`.ollama autopilot on|off <bot>` always overrides the rules. To turn a bot
off, use it rather than `nc -autopilot`.

### Controlling cost

A bot asks for a new plan in two cases:

- **Its last plan runs out.**
- **Something happens:** a level, a goal finished or stalled, an errand done
  or failed, a stuck walk, an alert (death streak, broken gear, full bags), or
  leaving a dungeon, battleground or group.

How often it may ask depends on its tier:

| Tier | When (defaults) | Asks at most every |
|---|---|---|
| foreground | a real player in the same zone, or a guildmate online | `DecisionIntervalMinutes` (15) |
| background | a real player on the same map | `BackgroundIntervalMinutes` (30) |
| dormant | nobody around | not asked; keeps its last orders |

All plans share `LlmCallsPerHour` (300). If you have the hardware or API
budget to give the LLM to more bots:

| Profile | `ForegroundScope` | `BackgroundScope` | `MinimumTier` |
|---|---|---|---|
| Light (default) | `zone` | `map` | `dormant` |
| Busy realm | `map` | `world` | `dormant` |
| Everyone thinks | `world` | `always` | `background` |

Raise `LlmCallsPerHour` and `MaxConcurrentPlans` to match.

Testing alone, for example with a selfbot? Bots count as nobody, so other
bots stay dormant away from you. Set `OllamaChat.Autopilot.MinimumTier =
"foreground"` (or `"background"`) to plan for every enrolled bot regardless.
`RealPlayerGuildTier` sets a floor for bots in a human's guild.
`Autopilot.Model` can send planning to a different model than chat.

### Commands

| Command | Does |
|---|---|
| `.ollama autopilot status [bot]` | Overview, or one bot's identity, plan, strategies, orders, goal and progress |
| `.ollama autopilot preview` | Dry-runs the selection rules: matches, tiers, LLM demand vs budget |
| `.ollama autopilot on\|off\|rules <bot>` | Force on, force off, or hand back to the rules |
| `.ollama autopilot history <bot> [n]` | The bot's diary: every plan, order, errand and event |
| `.ollama autopilot identity <bot> [clear]` | Show the identity, or have the LLM write a new one |
| `.ollama autopilot goal <bot> [kind target\|clear]` | Show or set a goal, e.g. `goal Bob reach_skill mining 150` |
| `.ollama autopilot replan <bot>` | Ask the LLM now |

### Making it yours

- `Autopilot.DeniedCommands` lists orders the LLM may never give. `nc +pvp`
  denies one change; `nc` denies all out-of-combat strategy changes.
- `Autopilot.CommandReference` replaces what the LLM is told it can order.
- `Autopilot.PromptTemplate` replaces the whole planning prompt.

Every option is documented in `mod_ollama_chat.conf.dist` under *AUTOPILOT*.
Design notes are in [`docs/autopilot-plan.md`](docs/autopilot-plan.md).

### Good to know

- Boats and zeppelins depend on the server's transports running. If a bot
  waits at a dock for `Travel.BoatWaitMinutes` and nothing comes, the trip
  fails and the LLM is told. The startup log line "indexed N boat/zeppelin
  crossings ..., N portal triggers and N city portals" shows what was found.
- A bot walks onto the pier before looking for the deck, and never boards
  blind: if no deck point is found, it waits for the next docking.
- Quest rewards are the LLM's choice (`quest <id> reward <n>`). If it turns
  a quest in without choosing, autopilot takes the usable choice with the
  highest item level.
- The Ollama Monitor addon (see *Debugging*) shows every enrolled bot live:
  its plan, trip, the model's last prompt and reply, its chat and memory.
- Decisions are only as good as your model. A small model may give thin
  identities or orders that don't fit. Each refused or failed order is shown
  to the LLM on its next plan and counted in `.ollama autopilot status`.

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
