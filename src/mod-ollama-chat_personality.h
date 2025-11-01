#ifndef MOD_OLLAMA_CHAT_PERSONALITY_H
#define MOD_OLLAMA_CHAT_PERSONALITY_H

#include <unordered_map>
#include <string>
#include <cstdint>
#include <vector>

class Player; // forward declaration

// Returns the personality key (as a string) assigned to the given bot.
// Will randomly assign from the loaded config if not yet set.
std::string GetBotPersonality(Player* bot);

// Given a personality key, returns the prompt addition string from config.
// Falls back to a default if not found.
std::string GetPersonalityPromptAddition(const std::string& type);

// Set a bot's personality manually (saves to database)
bool SetBotPersonality(Player* bot, const std::string& personality);

// Get all available personality keys
std::vector<std::string> GetAllPersonalityKeys();

// Check if a personality exists
bool PersonalityExists(const std::string& personality);

#endif // MOD_OLLAMA_CHAT_PERSONALITY_H
