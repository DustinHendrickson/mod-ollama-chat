#ifndef MOD_OLLAMA_CHAT_CONFIG_H
#define MOD_OLLAMA_CHAT_CONFIG_H

#include <string>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <ctime>
#include "ScriptMgr.h"  // Ensure WorldScript is defined

// --------------------------------------------
// Distance/Range Configuration
// --------------------------------------------
extern float      g_SayDistance;
extern float      g_YellDistance;
extern float      g_RandomChatterRealPlayerDistance;
extern float      g_EventChatterRealPlayerDistance;

// --------------------------------------------
// Bot/Player Chatter Probability & Limits
// --------------------------------------------
extern uint32_t   g_PlayerReplyChance;
extern uint32_t   g_BotReplyChance;
extern uint32_t   g_MaxBotsToPick;
extern uint32_t   g_RandomChatterBotCommentChance;
extern uint32_t   g_RandomChatterMaxBotsPerPlayer;
extern uint32_t   g_EventChatterBotCommentChance;
extern uint32_t   g_EventChatterBotSelfCommentChance;
extern uint32_t   g_EventChatterMaxBotsPerPlayer;

// --------------------------------------------
// Ollama LLM API Configuration
// --------------------------------------------
extern std::string g_OllamaUrl;
extern std::string g_OllamaModel;
extern uint32_t    g_OllamaNumPredict;
extern float       g_OllamaTemperature;
extern float       g_OllamaTopP;
extern float       g_OllamaRepeatPenalty;
extern uint32_t    g_OllamaNumCtx;
extern uint32_t    g_OllamaNumThreads;
extern std::string g_OllamaStop;
extern std::string g_OllamaSystemPrompt;
extern std::string g_OllamaSeed;

// --------------------------------------------
// Concurrency/Queueing
// --------------------------------------------
extern uint32_t    g_MaxConcurrentQueries;

// --------------------------------------------
// Feature Toggles & Core Settings
// --------------------------------------------
extern bool        g_Enable;
extern bool        g_DisableRepliesInCombat;
extern bool        g_EnableRandomChatter;
extern bool        g_EnableEventChatter;
extern bool        g_EnableRPPersonalities;
extern bool        g_DebugEnabled;
extern bool        g_DebugShowFullPrompt;

// --------------------------------------------
// Random Chatter Timing
// --------------------------------------------
extern uint32_t    g_MinRandomInterval;
extern uint32_t    g_MaxRandomInterval;

// --------------------------------------------
// Conversation History Settings
// --------------------------------------------
extern uint32_t    g_MaxConversationHistory;
extern uint32_t    g_ConversationHistorySaveInterval;

// --------------------------------------------
// Prompt Templates
// --------------------------------------------
extern std::string g_RandomChatterPromptTemplate;
extern std::string g_EventChatterPromptTemplate;
extern std::string g_ChatPromptTemplate;
extern std::string g_ChatExtraInfoTemplate;

// --------------------------------------------
// Personality and Prompt Data
// --------------------------------------------
extern std::unordered_map<uint64_t, std::string> g_BotPersonalityList;
extern std::unordered_map<std::string, std::string> g_PersonalityPrompts;
extern std::vector<std::string> g_PersonalityKeys;
extern std::string g_DefaultPersonalityPrompt;

// --------------------------------------------
// Chat History Templates and Toggles
// --------------------------------------------
extern bool        g_EnableChatHistory;
extern std::string g_ChatHistoryHeaderTemplate;
extern std::string g_ChatHistoryLineTemplate;
extern std::string g_ChatHistoryFooterTemplate;

// --------------------------------------------
// Chatbot Snapshot Template
// --------------------------------------------
extern bool        g_EnableChatBotSnapshotTemplate;
extern std::string g_ChatBotSnapshotTemplate;

// --------------------------------------------
// Conversation History Store and Mutex
// --------------------------------------------
extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>>> g_BotConversationHistory;
extern std::mutex   g_ConversationHistoryMutex;
extern time_t       g_LastHistorySaveTime;

// --------------------------------------------
// Blacklist: Prefixes for Commands (not chat)
// --------------------------------------------
extern std::vector<std::string> g_BlacklistCommands;

// --------------------------------------------
// Think Mode Support
// --------------------------------------------
extern bool g_ThinkModeEnableForModule;

// --------------------------------------------
// Environment/Contextual Random Chatter Templates
// --------------------------------------------
extern std::vector<std::string> g_EnvCommentCreature;
extern std::vector<std::string> g_EnvCommentGameObject;
extern std::vector<std::string> g_EnvCommentEquippedItem;
extern std::vector<std::string> g_EnvCommentBagItem;
extern std::vector<std::string> g_EnvCommentBagItemSell;
extern std::vector<std::string> g_EnvCommentSpell;
extern std::vector<std::string> g_EnvCommentQuestArea;
extern std::vector<std::string> g_EnvCommentVendor;
extern std::vector<std::string> g_EnvCommentQuestgiver;
extern std::vector<std::string> g_EnvCommentBagSlots;
extern std::vector<std::string> g_EnvCommentDungeon;
extern std::vector<std::string> g_EnvCommentUnfinishedQuest;

// --------------------------------------------
// Guild-Specific Random Chatter Templates
// --------------------------------------------
extern std::vector<std::string> g_GuildEnvCommentGuildMember;
extern std::vector<std::string> g_GuildEnvCommentGuildRank;
extern std::vector<std::string> g_GuildEnvCommentGuildBank;
extern std::vector<std::string> g_GuildEnvCommentGuildMOTD;
extern std::vector<std::string> g_GuildEnvCommentGuildInfo;
extern std::vector<std::string> g_GuildEnvCommentGuildOnlineMembers;
extern std::vector<std::string> g_GuildEnvCommentGuildRaid;
extern std::vector<std::string> g_GuildEnvCommentGuildEndgame;
extern std::vector<std::string> g_GuildEnvCommentGuildStrategy;
extern std::vector<std::string> g_GuildEnvCommentGuildGroup;
extern std::vector<std::string> g_GuildEnvCommentGuildPvP;
extern std::vector<std::string> g_GuildEnvCommentGuildCommunity;

// --------------------------------------------
// Guild-Specific Random Chatter Configuration
// --------------------------------------------
extern bool        g_EnableGuildEventChatter;
extern bool        g_EnableGuildRandomAmbientChatter;
extern uint32_t    g_GuildRandomChatterChance;
extern uint32_t    g_GuildChatterBotCommentChance;
extern uint32_t    g_GuildChatterMaxBotsPerEvent;

// --------------------------------------------
// Guild-Specific Event Chatter Templates
// --------------------------------------------
extern std::string g_GuildEventTypeLevelUp;
extern std::string g_GuildEventTypeDungeonComplete;
extern std::string g_GuildEventTypeEpicGear;
extern std::string g_GuildEventTypeRareGear;
extern std::string g_GuildEventTypeGuildJoin;
extern std::string g_GuildEventTypeGuildLeave;
extern std::string g_GuildEventTypeGuildPromotion;
extern std::string g_GuildEventTypeGuildDemotion;
extern std::string g_GuildEventTypeGuildLogin;
extern std::string g_GuildEventTypeGuildAchievement;

// Chance variables for normal events
extern int g_EventTypeDefeated_Chance;
extern int g_EventTypeDefeatedPlayer_Chance;
extern int g_EventTypePetDefeated_Chance;
extern int g_EventTypeGotItem_Chance;
extern int g_EventTypeDied_Chance;
extern int g_EventTypeCompletedQuest_Chance;
extern int g_EventTypeLearnedSpell_Chance;
extern int g_EventTypeRequestedDuel_Chance;
extern int g_EventTypeStartedDueling_Chance;
extern int g_EventTypeWonDuel_Chance;
extern int g_EventTypeLeveledUp_Chance;
extern int g_EventTypeAchievement_Chance;
extern int g_EventTypeUsedObject_Chance;

// Chance variables for guild events
extern int g_GuildEventTypeEpicGear_Chance;
extern int g_GuildEventTypeRareGear_Chance;
extern int g_GuildEventTypeGuildJoin_Chance;
extern int g_GuildEventTypeGuildLogin_Chance;
extern int g_GuildEventTypeGuildLeave_Chance;
extern int g_GuildEventTypeGuildPromotion_Chance;
extern int g_GuildEventTypeGuildDemotion_Chance;
extern int g_GuildEventTypeGuildAchievement_Chance;
extern int g_GuildEventTypeLevelUp_Chance;
extern int g_GuildEventTypeDungeonComplete_Chance;

// --------------------------------------------
// Bot-Player Sentiment Tracking System
// --------------------------------------------
extern bool        g_EnableSentimentTracking;
extern float       g_SentimentDefaultValue;              // Default sentiment value (0.5 = neutral)
extern float       g_SentimentAdjustmentStrength;        // How much to adjust sentiment per message (0.1)
extern uint32_t    g_SentimentSaveInterval;              // How often to save sentiment to DB (minutes)
extern std::string g_SentimentAnalysisPrompt;            // Prompt template for sentiment analysis
extern std::string g_SentimentPromptTemplate;            // Template for including sentiment in bot prompts

// In-memory sentiment storage and mutex
extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, float>> g_BotPlayerSentiments;
extern std::mutex g_SentimentMutex;
extern time_t g_LastSentimentSaveTime;

// --------------------------------------------
// RAG (Retrieval-Augmented Generation) System
// --------------------------------------------
extern bool        g_EnableRAG;                          // Enable/disable RAG feature
extern std::string g_RAGDataPath;                        // Path to RAG data files
extern uint32_t    g_RAGMaxRetrievedItems;               // Max items to retrieve
extern float       g_RAGSimilarityThreshold;             // Similarity threshold for retrieval
extern std::string g_RAGPromptTemplate;                  // Template for RAG info in prompts

class OllamaRAGSystem;
extern OllamaRAGSystem* g_RAGSystem;                     // Global RAG system instance

// --------------------------------------------
// Event Chatter: Event Type Strings
// These control the event type string sent to eventChatter for world event prompts.
// Values are loaded from conf (see mod_ollama_chat.conf.dist)
// --------------------------------------------
extern std::string g_EventTypeDefeated;           // "defeated"
extern std::string g_EventTypeDefeatedPlayer;     // "defeated player"
extern std::string g_EventTypePetDefeated;        // "pet defeated"
extern std::string g_EventTypeGotItem;            // "got item"
extern std::string g_EventTypeDied;               // "died"
extern std::string g_EventTypeCompletedQuest;     // "completed quest"
extern std::string g_EventTypeLearnedSpell;       // "learned spell"
extern std::string g_EventTypeRequestedDuel;      // "requested to duel"
extern std::string g_EventTypeStartedDueling;     // "started dueling"
extern std::string g_EventTypeWonDuel;            // "won duel against"
extern std::string g_EventTypeLeveledUp;          // "leveled up"
extern std::string g_EventTypeAchievement;        // "earned achievement"
extern std::string g_EventTypeUsedObject;         // "used object"

// Event Cooldown
extern uint32_t g_EventCooldownTime;

// --------------------------------------------
// Party Restriction Settings
// --------------------------------------------
extern bool g_RestrictBotsToPartyMembers;

// --------------------------------------------
// Loader Functions
// --------------------------------------------
void LoadOllamaChatConfig();
void LoadBotPersonalityList();
void LoadBotConversationHistoryFromDB();
void LoadPersonalityTemplatesFromDB();

// --------------------------------------------
// Declaration of the configuration WorldScript.
// --------------------------------------------
class OllamaChatConfigWorldScript : public WorldScript
{
public:
    OllamaChatConfigWorldScript();
    void OnStartup() override;
    void OnShutdown() override;
};

#endif // MOD_OLLAMA_CHAT_CONFIG_H
