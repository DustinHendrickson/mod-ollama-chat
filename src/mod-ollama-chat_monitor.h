#ifndef MOD_OLLAMA_CHAT_MONITOR_H
#define MOD_OLLAMA_CHAT_MONITOR_H

#include "ScriptMgr.h"

#include <cstdint>

// --------------------------------------------------------------------------
// Monitor: the server side of the OllamaMonitor client addon (addon/).
//
// The addon whispers its own character on the addon channel ("OAPM\t<request>")
// and gets its answers the same way, as CHAT_MSG_ADDON events -- no client
// patch. Requests: the list of autopilot bots, one page of debug detail for a
// bot, and a camera that follows a bot (the core's farsight viewpoint, which
// Mind Vision uses; the watcher is teleported along when the bot changes map).
//
// Gated by OllamaChat.Monitor.Enable and .MinSecurity.
// --------------------------------------------------------------------------

void Monitor_LoadConfig();

// World tick: keeps followed cameras on their bots.
void Monitor_Update(uint32_t diff);

class OllamaMonitorScript : public PlayerScript
{
public:
    OllamaMonitorScript();

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override;
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z, float orientation,
                                uint32 options, Unit* target) override;
    void OnPlayerLogout(Player* player) override;
};

#endif // MOD_OLLAMA_CHAT_MONITOR_H
