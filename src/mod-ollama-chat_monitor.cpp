#include "mod-ollama-chat_monitor.h"
#include "mod-ollama-chat_autopilot.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_progress.h"
#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat_topics.h"
#include "mod-ollama-chat_world.h"
#include "mod-ollama-chat-utilities.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

// Protocol (both directions are addon whispers to oneself, prefix "OAPM"):
//
//   client -> server                 server -> client
//   HELLO                            H \t <version>
//   LIST                             LB | L \t guid \t name \t level \t class \t zone \t tier \t state \t doing | LZ \t n
//   PAGE <guid> <page>               B \t guid \t page | P \t c|e \t text ... | Z \t guid \t page
//   WATCH <guid> / UNWATCH           W \t on|off|wait \t guid \t name \t note
//   GOTO <guid>                      M \t text
//   CMD on|off|replan|status <name>  (system messages, as the .ollama command)
//                                    G \t guid   (a PAGE for a bot no longer online)
//
// Page lines longer than one message are split; "c" marks a piece the next
// one continues, "e" the end of a line.

namespace
{
    constexpr const char* kPrefix     = "OAPM";
    constexpr const char* kVersion    = "1";
    constexpr size_t      kPieceBytes = 220;   // payload per message; the client takes 255 in all

    struct MonitorConfig
    {
        bool     enable      = true;
        uint32_t minSecurity = SEC_ADMINISTRATOR;
    };
    MonitorConfig g_mc;

    // Cameras following bots: watcher raw guid -> watch. The world thread
    // owns it; the teleport hook (a map thread) only clears viewpoints of
    // watchers standing on the bot's own map, which that thread is updating.
    struct Watch
    {
        uint64_t bot           = 0;
        uint32_t lastTeleport  = 0;
        bool     noteInstance  = false;   // told the watcher a dungeon can't be followed
    };
    std::mutex                              g_watchMutex;
    std::unordered_map<uint64_t, Watch>     g_watches;
    uint32_t                                g_watchTimer = 0;

    // ----------------------------------------------------------------------
    // Sending
    // ----------------------------------------------------------------------

    void Send(Player* to, const std::string& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, to, to, std::string(kPrefix) + "\t" + body);
        to->GetSession()->SendPacket(&data);
    }

    // Free text inside a tab-separated message: no tabs, newlines or the
    // client's escape character.
    std::string Clean(std::string s)
    {
        for (char& c : s)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
            else if (c == '|')
                c = '/';
        return s;
    }

    std::string Cut(std::string s, size_t bytes)
    {
        if (s.size() <= bytes)
            return s;
        size_t end = bytes;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
            --end;   // not inside a UTF-8 sequence
        s.resize(end);
        return s + "...";
    }

    void SendPage(Player* to, uint64_t guid, const std::string& page, const std::vector<std::string>& lines)
    {
        Send(to, SafeFormat("B\t{}\t{}", guid, page));
        for (const std::string& raw : lines)
        {
            const std::string line = Clean(raw);
            size_t pos = 0;
            do
            {
                size_t len = std::min(kPieceBytes, line.size() - pos);
                while (len > 0 && pos + len < line.size() &&
                       (static_cast<unsigned char>(line[pos + len]) & 0xC0) == 0x80)
                    --len;
                if (len == 0)   // not UTF-8 at all: cut anywhere rather than never advance
                    len = std::min(kPieceBytes, line.size() - pos);
                const bool last = pos + len >= line.size();
                Send(to, std::string("P\t") + (last ? "e" : "c") + "\t" + line.substr(pos, len));
                pos += len;
            } while (pos < line.size());
        }
        Send(to, SafeFormat("Z\t{}\t{}", guid, page));
    }

    void Note(Player* to, const std::string& text) { Send(to, "M\t" + Clean(text)); }

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    bool Allowed(Player* player)
    {
        return g_mc.enable && player->GetSession() && player->GetSession()->GetSecurity() >= g_mc.minSecurity;
    }

    std::string NameOf(uint64_t guid)
    {
        if (!guid)
            return "-";
        std::string name;
        if (sCharacterCache->GetCharacterNameByGuid(ObjectGuid(guid), name))
            return name;
        return SafeFormat("guid {}", ObjectGuid(guid).GetCounter());
    }

    std::string Ago(uint32_t then, uint32_t now)
    {
        const uint32_t s = now > then ? now - then : 0;
        if (s < 120)       return SafeFormat("{}s ago", s);
        if (s < 2 * 3600)  return SafeFormat("{}m ago", s / 60);
        if (s < 2 * 86400) return SafeFormat("{}h ago", s / 3600);
        return SafeFormat("{}d ago", s / 86400);
    }

    std::string Seconds(double s)
    {
        return s < 0.0 ? std::string("never") : SafeFormat("{:.0f}s ago", s);
    }

    void Head(std::vector<std::string>& out, const std::string& title) { out.push_back("# " + title); }
    void Kv(std::vector<std::string>& out, const std::string& key, const std::string& value)
    {
        out.push_back(key + ": " + (value.empty() ? std::string("-") : value));
    }

    void TextBlock(std::vector<std::string>& out, const std::string& text)
    {
        std::istringstream in(text);
        std::string line;
        bool any = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.push_back("  " + line);
            any = true;
        }
        if (!any)
            out.push_back("  (none)");
    }

    Player* FindBot(uint64_t guid)
    {
        // Bots only: the monitor never follows, pages or teleports to people.
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        return bot && bot->IsInWorld() && OllamaIsBotPlayer(bot) ? bot : nullptr;
    }

    // ----------------------------------------------------------------------
    // Chat pages
    // ----------------------------------------------------------------------

    void ChatPage(Player* bot, std::vector<std::string>& out)
    {
        const uint64_t guid = bot->GetGUID().GetRawValue();
        const uint32_t now  = Progress_Now();

        Head(out, "Chat");
        std::string personality;   // read only: GetBotPersonality would assign one
        {
            std::lock_guard<std::mutex> lock(g_BotPersonalityMutex);
            if (auto pers = g_BotPersonalityList.find(guid); pers != g_BotPersonalityList.end())
                personality = pers->second;
        }
        Kv(out, "personality", personality.empty() ? std::string("none assigned yet") : personality);
        if (!personality.empty())
            if (auto p = g_PersonalityPrompts.find(personality); p != g_PersonalityPrompts.end())
                Kv(out, "personality prompt", p->second);
        Kv(out, "roleplay voice", Roleplay_BuildVoicePrompt(bot));
        Kv(out, "replies queued", std::to_string(OllamaDispatch_PendingFor(guid)));

        GovernorBotDebug gov;
        if (Governor_GetBotDebug(bot->GetGUID(), gov))
        {
            Kv(out, "last spoke", SafeFormat("{} (bot cooldown {}s)", Seconds(gov.sinceSend), g_BotCooldownSeconds));
            Kv(out, "last event line", Seconds(gov.sinceEvent));
            Kv(out, "lines remembered", SafeFormat("{} (repetition check)", gov.remembered));
            for (const GovernorConversation& c : gov.conversations)
                Kv(out, "in conversation", SafeFormat("{} in {} ({})", NameOf(c.playerGuid), c.scopeKey,
                                                      Seconds(c.secondsAgo)));
        }
        else
            Kv(out, "last spoke", "never this session");

        const time_t next = OllamaRandomChatter_NextTime(bot->GetGUID());
        Kv(out, "next random chatter", next == 0 ? std::string("not scheduled")
                                       : next > time_t(now) ? SafeFormat("in {}s", uint32_t(next - now))
                                                            : std::string("due"));

        OllamaChatTrace trace;
        const bool haveTrace = OllamaDispatch_GetTrace(guid, trace);

        Head(out, "Recent replies (newest last)");
        if (!haveTrace || trace.lines.empty())
            out.push_back("  none this session");
        else
            for (const OllamaChatTraceLine& l : trace.lines)
            {
                std::string where = ChatChannelSourceLocalStr[l.source];
                if (!l.channelName.empty())
                    where += " " + l.channelName;
                if (l.targetGuid)
                    where += " -> " + NameOf(l.targetGuid);
                out.push_back(SafeFormat("[{}] {} (depth {}): {}", Ago(l.at, now), where, l.chainDepth, l.outcome));
                if (!l.heard.empty())
                    out.push_back("    heard: " + l.heard);
                if (!l.text.empty())
                    out.push_back("    said: " + l.text);
            }

        Head(out, "Conversation history kept for prompts");
        {
            std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
            auto it = g_BotConversationHistory.find(guid);
            if (it == g_BotConversationHistory.end() || it->second.empty())
                out.push_back("  none");
            else
                for (const auto& [player, entries] : it->second)
                {
                    out.push_back(SafeFormat("  with {} ({} exchanges)", NameOf(player), entries.size()));
                    for (const BotConversationEntry& e : entries)
                    {
                        out.push_back("    them: " + e.playerMessage);
                        out.push_back("    bot: " + e.botReply);
                    }
                }
        }

        if (haveTrace && trace.lastAt)
        {
            Head(out, SafeFormat("Last chat reply from the model ({}, {} ms)", Ago(trace.lastAt, now),
                                 trace.lastLatencyMs));
            TextBlock(out, trace.lastRaw);
            Head(out, "Last chat prompt");
            TextBlock(out, trace.lastPrompt);
        }
    }

    void MindPage(Player* bot, std::vector<std::string>& out)
    {
        const uint64_t guid = bot->GetGUID().GetRawValue();
        const uint32_t now  = Progress_Now();

        BotMemoryDebug mem;
        Memory_GetDebug(guid, mem);
        Head(out, SafeFormat("Memories ({}{})", mem.memories.size(), mem.condensing ? ", condensing now" : ""));
        Kv(out, "raw history held", SafeFormat("{} tokens", mem.historyTokens));
        std::vector<BotMemoryEntry> memories = mem.memories;
        std::sort(memories.begin(), memories.end(),
                  [](const BotMemoryEntry& a, const BotMemoryEntry& b) { return a.createdAt > b.createdAt; });
        for (const BotMemoryEntry& m : memories)
            out.push_back(SafeFormat("  [{}] ({}) {}", m.importance, m.createdAt ? Ago(uint32_t(m.createdAt), now)
                                                                                 : std::string("-"), m.text));

        Head(out, "Relationships");
        if (mem.relationships.empty())
            out.push_back("  none");
        for (const BotRelationship& r : mem.relationships)
            out.push_back(SafeFormat("  {} ({} mentions, {}): {}",
                                     r.otherName.empty() ? NameOf(r.otherGuid) : r.otherName, r.mentions,
                                     r.updatedAt ? Ago(uint32_t(r.updatedAt), now) : std::string("-"),
                                     r.description));

        Head(out, "Sentiment toward players (0 hostile .. 1 friendly)");
        {
            std::vector<std::pair<uint64_t, float>> feelings;
            {
                std::lock_guard<std::mutex> lock(g_SentimentMutex);
                if (auto it = g_BotPlayerSentiments.find(guid); it != g_BotPlayerSentiments.end())
                    feelings.assign(it->second.begin(), it->second.end());
            }
            if (feelings.empty())
                out.push_back("  none");
            for (const auto& [player, value] : feelings)
                out.push_back(SafeFormat("  {}: {:.2f}", NameOf(player), value));
        }

        std::vector<std::string> keys;
        std::vector<std::pair<std::string, uint32_t>> witnessed;
        Topics_GetDebug(bot->GetGUID(), keys, witnessed);
        Head(out, "Topics used lately (oldest first)");
        std::string joined;
        for (const std::string& k : keys)
            joined += (joined.empty() ? "" : ", ") + k;
        out.push_back("  " + (joined.empty() ? std::string("none") : joined));
        Head(out, "Witnessed events");
        if (witnessed.empty())
            out.push_back("  none");
        for (const auto& [text, age] : witnessed)
            out.push_back(SafeFormat("  [{}s ago] {}", age, text));
    }

    // ----------------------------------------------------------------------
    // Camera
    // ----------------------------------------------------------------------

    // Watchers whose old viewpoint could not be found from a map thread; the
    // world tick clears them (it may touch the seer on whatever map it is).
    std::vector<uint64_t> g_pendingRelease;   // under g_watchMutex

    // Give the watcher its own camera back. GetViewpoint only finds the seer on
    // the watcher's own map, and while PLAYER_FARSIGHT still holds a guid the
    // core refuses any new viewpoint (AddGuidValue), so a seer that moved to
    // another map, or logged out, used to leave the camera stuck for good.
    // From a map thread (onWorldThread = false) such a case is deferred.
    void ReleaseView(Player* watcher, bool onWorldThread = true)
    {
        if (WorldObject* view = watcher->GetViewpoint())
        {
            watcher->SetViewpoint(view, false);
            return;
        }

        const ObjectGuid stale = watcher->GetGuidValue(PLAYER_FARSIGHT);
        if (!stale)
        {
            if (watcher->GetSeer() != watcher)
                watcher->SetSeer(watcher);
            return;
        }

        if (!onWorldThread)
        {
            std::lock_guard<std::mutex> lock(g_watchMutex);
            g_pendingRelease.push_back(watcher->GetGUID().GetRawValue());
            return;
        }

        // What SetViewpoint(view, false) does, without needing to find it here.
        watcher->SetSeer(watcher);
        if (stale.IsPlayer() && !watcher->GetVehicle())
            if (Player* seer = ObjectAccessor::FindConnectedPlayer(stale))
                seer->RemovePlayerFromVision(watcher);
        watcher->SetGuidValue(PLAYER_FARSIGHT, ObjectGuid::Empty);
    }

    void SendWatch(Player* watcher, const char* state, uint64_t bot, const std::string& note)
    {
        Send(watcher, SafeFormat("W\t{}\t{}\t{}\t{}", state, bot, Clean(NameOf(bot)), Clean(note)));
    }

    void StopWatching(Player* watcher, const std::string& note)
    {
        uint64_t bot = 0;
        {
            std::lock_guard<std::mutex> lock(g_watchMutex);
            auto it = g_watches.find(watcher->GetGUID().GetRawValue());
            if (it != g_watches.end())
            {
                bot = it->second.bot;
                g_watches.erase(it);
            }
        }
        // Always release and always answer: the addon's button must never be
        // left saying "Stop camera" with nothing on the server to stop.
        ReleaseView(watcher);
        SendWatch(watcher, "off", bot, note);
    }

    void StepWatch(uint64_t watcherGuid, Watch& w, uint32_t now, std::vector<uint64_t>& finished)
    {
        Player* watcher = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watcherGuid));
        if (!watcher || !watcher->IsInWorld())
        {
            finished.push_back(watcherGuid);
            return;
        }
        if (watcher->IsBeingTeleported())
            return;

        Player* bot = FindBot(w.bot);
        if (!bot)
        {
            ReleaseView(watcher);
            SendWatch(watcher, "off", w.bot, "the bot went offline");
            finished.push_back(watcherGuid);
            return;
        }
        if (bot->IsBeingTeleported())
            return;

        if (watcher->GetMap() != bot->GetMap())
        {
            ReleaseView(watcher);
            if (bot->GetMap()->Instanceable())
            {
                // An instance id cannot be chosen through TeleportTo: the
                // watcher would land in an instance of their own.
                if (!w.noteInstance)
                    SendWatch(watcher, "wait", w.bot, "inside an instance - the camera waits outside until it leaves");
                w.noteInstance = true;
                return;
            }
            if (now - w.lastTeleport >= 5)
            {
                w.lastTeleport = now;
                SendWatch(watcher, "wait", w.bot, "following to " + Progress_ZoneName(bot->GetZoneId()));
                watcher->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                    bot->GetPositionZ() + 2.0f, bot->GetOrientation());
            }
            return;
        }

        w.noteInstance = false;
        if (watcher->GetViewpoint() != bot)
        {
            ReleaseView(watcher);
            watcher->SetViewpoint(bot, true);
            SendWatch(watcher, "on", w.bot, "camera on " + bot->GetName());
        }
    }

    // ----------------------------------------------------------------------
    // Requests
    // ----------------------------------------------------------------------

    uint64_t ParseGuid(const std::string& text)
    {
        try { return std::stoull(text); }
        catch (...) { return 0; }
    }

    void HandleRequest(Player* player, const std::string& request)
    {
        std::istringstream in(request);
        std::string verb;
        in >> verb;

        if (verb == "HELLO")
        {
            Send(player, SafeFormat("H\t{}\t{}", kVersion, Autopilot_IsActive() ? "active" : "inactive"));
            for (const std::string& why : Autopilot_InactiveReasons())
                Send(player, "M\tAutopilot is off: " + Clean(why));

            // After a /reload the client has forgotten a camera the server
            // still holds: tell it.
            uint64_t watched = 0;
            {
                std::lock_guard<std::mutex> lock(g_watchMutex);
                if (auto it = g_watches.find(player->GetGUID().GetRawValue()); it != g_watches.end())
                    watched = it->second.bot;
            }
            if (watched)
                SendWatch(player, "on", watched, "camera on " + NameOf(watched));
            return;
        }

        if (verb == "LIST")
        {
            std::vector<AutopilotMonitorRow> rows = Autopilot_MonitorList();
            Send(player, "LB");
            for (const AutopilotMonitorRow& r : rows)
                Send(player, SafeFormat("L\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", r.guid, Clean(r.name), r.level, r.cls,
                                        Clean(Cut(Progress_ZoneName(r.zone), 40)), r.tier, Clean(r.state),
                                        Clean(Cut(r.doing, 100))));
            Send(player, SafeFormat("LZ\t{}", rows.size()));
            return;
        }

        // The window's buttons. Gated by Monitor.MinSecurity like the rest of
        // the window, not by the GM level of the .ollama chat commands.
        if (verb == "CMD")
        {
            std::string sub, name;
            in >> sub >> name;
            if (!Autopilot_MonitorCommand(player, sub, name))
                Send(player, "M\tUnknown monitor command: " + Clean(sub));
            return;
        }

        std::string guidText;
        in >> guidText;
        const uint64_t guid = ParseGuid(guidText);

        if (verb == "UNWATCH")
        {
            StopWatching(player, "stopped");
            return;
        }

        Player* bot = guid ? FindBot(guid) : nullptr;
        if (!bot)
        {
            // A page is the addon polling, not the user asking: answer quietly
            // so it can stop, instead of a chat line per poll.
            if (verb == "PAGE")
                Send(player, SafeFormat("G\t{}", guid));
            else
                Note(player, "That bot is not online.");
            return;
        }

        if (verb == "PAGE")
        {
            std::string page;
            in >> page;
            std::vector<std::string> lines;
            if (page == "chat")
                ChatPage(bot, lines);
            else if (page == "mind")
                MindPage(bot, lines);
            else if (!Autopilot_MonitorPage(bot, page, lines))
                lines.push_back(bot->GetName() + " has never been on autopilot.");
            SendPage(player, guid, page, lines);
            return;
        }

        if (verb == "WATCH")
        {
            if (bot == player)
                return;
            {
                std::lock_guard<std::mutex> lock(g_watchMutex);
                Watch& w = g_watches[player->GetGUID().GetRawValue()];
                w = Watch();
                w.bot = guid;
            }
            ReleaseView(player);
            SendWatch(player, "wait", guid, "finding " + bot->GetName());
            g_watchTimer = 1000;   // step it on the next tick
            return;
        }

        if (verb == "GOTO")
        {
            if (bot->GetMap()->Instanceable())
            {
                Note(player, bot->GetName() + " is inside an instance.");
                return;
            }
            // Going somewhere yourself ends any camera follow; otherwise the
            // follow tick would pull you straight back to the bot you watched.
            StopWatching(player, "you teleported to " + bot->GetName());
            player->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 1.0f,
                               bot->GetOrientation());
            return;
        }

        Note(player, "Unknown request: " + verb);
    }
}

void Monitor_LoadConfig()
{
    MonitorConfig c;
    c.enable      = sConfigMgr->GetOption<bool>("OllamaChat.Monitor.Enable", true);
    c.minSecurity = std::min<uint32_t>(SEC_CONSOLE,
                                       sConfigMgr->GetOption<uint32_t>("OllamaChat.Monitor.MinSecurity", SEC_ADMINISTRATOR));
    g_mc = c;
}

void Monitor_Update(uint32_t diff)
{
    g_watchTimer += diff;
    if (g_watchTimer < 1000)
        return;
    g_watchTimer = 0;

    // Cameras a map thread could not release (their seer was elsewhere).
    std::vector<uint64_t> pending;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        pending.swap(g_pendingRelease);
    }
    for (uint64_t guid : pending)
        if (Player* watcher = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)))
        {
            if (watcher->IsInWorld() && !watcher->IsBeingTeleported())
                ReleaseView(watcher);
            else
            {
                std::lock_guard<std::mutex> lock(g_watchMutex);   // mid-teleport: next tick
                g_pendingRelease.push_back(guid);
            }
        }

    const uint32_t now = uint32_t(std::time(nullptr));
    std::vector<std::pair<uint64_t, Watch>> watches;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        if (g_watches.empty())
            return;
        watches.assign(g_watches.begin(), g_watches.end());
    }

    // Stepped outside the lock: a teleport started here runs our own
    // teleport hook, which takes it.
    std::vector<uint64_t> finished;
    for (auto& [watcher, w] : watches)
        StepWatch(watcher, w, now, finished);

    std::lock_guard<std::mutex> lock(g_watchMutex);
    for (const auto& [watcher, w] : watches)
        if (auto it = g_watches.find(watcher); it != g_watches.end() && it->second.bot == w.bot)
        {
            it->second.lastTeleport = w.lastTeleport;
            it->second.noteInstance = w.noteInstance;
        }
    for (uint64_t watcher : finished)
        g_watches.erase(watcher);
}

OllamaMonitorScript::OllamaMonitorScript()
    : PlayerScript("OllamaMonitorScript", {
          PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
          PLAYERHOOK_ON_BEFORE_TELEPORT,
          PLAYERHOOK_ON_LOGOUT,
      })
{
}

bool OllamaMonitorScript::OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                             Player* receiver)
{
    if (language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player || !player)
        return true;
    if (msg.compare(0, 5, "OAPM\t") != 0)
        return true;

    if (!Allowed(player))
    {
        if (msg.compare(5, 5, "HELLO") == 0)
            Send(player, g_mc.enable ? "M\tYour account may not use the Ollama monitor (OllamaChat.Monitor.MinSecurity)."
                                     : "M\tThe Ollama monitor is turned off (OllamaChat.Monitor.Enable).");
        return false;
    }

    HandleRequest(player, msg.substr(5));
    return false;   // swallow the self-whisper
}

// A viewpoint must never outlive the bot's place on the watcher's map: the
// watcher keeps a raw pointer to it (Player::m_seer). Release it before the
// bot leaves -- this hook runs on the map thread updating the bot, which is
// the thread updating any watcher on that same map.
bool OllamaMonitorScript::OnPlayerBeforeTeleport(Player* player, uint32 /*mapid*/, float /*x*/, float /*y*/,
                                                 float /*z*/, float /*orientation*/, uint32 /*options*/,
                                                 Unit* /*target*/)
{
    if (!player)
        return true;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::vector<uint64_t> watchers;
    bool isWatcher = false;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        if (g_watches.empty())
            return true;
        isWatcher = g_watches.count(guid) > 0;
        for (const auto& [watcher, w] : g_watches)
            if (w.bot == guid)
                watchers.push_back(watcher);
    }

    if (isWatcher)
        ReleaseView(player, false);   // map thread; re-applied once the watcher has arrived

    for (uint64_t watcherGuid : watchers)
    {
        Player* watcher = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watcherGuid));
        if (watcher && watcher->IsInWorld() && watcher->GetMap() == player->GetMap() &&
            watcher->GetGuidValue(PLAYER_FARSIGHT) == player->GetGUID())
            watcher->SetViewpoint(player, false);
    }
    return true;
}

void OllamaMonitorScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::vector<uint64_t> watchers;
    bool isWatcher = false;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        if (g_watches.empty())
            return;
        isWatcher = g_watches.erase(guid) > 0;
        for (const auto& [watcher, w] : g_watches)
            if (w.bot == guid)
                watchers.push_back(watcher);
    }

    if (isWatcher)
        ReleaseView(player);

    for (uint64_t watcherGuid : watchers)
    {
        Player* watcher = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watcherGuid));
        if (!watcher || !watcher->IsInWorld() || watcher->GetGuidValue(PLAYER_FARSIGHT) != player->GetGUID())
            continue;
        // A bot can log out from its own map thread (playerbots' logout):
        // only a watcher on that map may be touched from here; one elsewhere
        // is released on the world thread.
        if (watcher->GetMap() == player->GetMap())
            watcher->SetViewpoint(player, false);
        else
            ReleaseView(watcher, false);
    }
}
