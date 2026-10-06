#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_GROUP_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_GROUP_H

// Groups for autopilot bots: the model invites, answers invites, leaves and
// shares quests, and whispers people (to ask first, or to answer). Every
// group change goes through the client's own packet handlers, the same way a
// player's buttons do. World thread only.

#include <string>

class Player;
class PlayerbotAI;

// "group ...", "invite <name>" or "whisper <name> <text>".
bool AutopilotGroup_IsOrder(const std::string& command);

// Runs one order. Results start with "done" when it went through. A whisper
// that went out names its receiver in `whisperedTo`.
std::string AutopilotGroup_Run(Player* bot, PlayerbotAI* ai, const std::string& command,
                               std::string& whisperedTo);

// The leader of the group that invited the bot, or nullptr.
Player* AutopilotGroup_Inviter(Player* bot);

// Group members on the leader's map, alive and on foot, between `minDist` and
// `maxDist` yards away: the farthest such distance, or 0 for none.
float AutopilotGroup_Straggler(Player* leader, float minDist, float maxDist);

#endif
