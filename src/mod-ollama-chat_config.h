#ifndef MOD_OLLAMA_CHAT_CONFIG_H
#define MOD_OLLAMA_CHAT_CONFIG_H

#include <string>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <set>
#include <utility>
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
// Per-channel-type reply chances
extern uint32_t   g_PlayerReplyChance_Say;
extern uint32_t   g_BotReplyChance_Say;
extern uint32_t   g_PlayerReplyChance_Channel;
extern uint32_t   g_BotReplyChance_Channel;
extern uint32_t   g_PlayerReplyChance_Party;
extern uint32_t   g_BotReplyChance_Party;
extern uint32_t   g_PlayerReplyChance_Guild;
extern uint32_t   g_BotReplyChance_Guild;

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

// Optional sampling controls for response diversity. All default to "unset",
// in which case the field is not sent at all and the model's default applies.
extern int32_t     g_OllamaTopK;              // -1 = unset
extern float       g_OllamaMinP;              // -1 = unset
extern float       g_OllamaPresencePenalty;   // <= -999 = unset
extern float       g_OllamaFrequencyPenalty;  // <= -999 = unset

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
extern bool        g_EnableWhisperReplies;
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
extern std::vector<std::string> g_RandomChatterPromptVariations;
extern std::vector<std::string> g_RandomChatterQuestionVariations;
extern std::string g_EventChatterPromptTemplate;
extern std::string g_ChatPromptTemplate;
extern std::string g_ChatExtraInfoTemplate;

// --------------------------------------------
// Personality and Prompt Data
// --------------------------------------------
extern std::unordered_map<uint64_t, std::string> g_BotPersonalityList;
extern std::unordered_map<std::string, std::string> g_PersonalityPrompts;
extern std::vector<std::string> g_PersonalityKeys;
extern std::vector<std::string> g_PersonalityKeysRandomOnly; // Personalities that can be randomly assigned
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
// One turn of a bot/player conversation.
//
// `persisted` is what keeps SaveBotConversationHistoryToDB() from rewriting
// the whole window every save interval. It is set once the row has been handed
// to the database, and is false for anything appended since the last save.
struct BotConversationEntry
{
    std::string playerMessage;
    std::string botReply;
    bool        persisted = false;
};

extern std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<BotConversationEntry>>> g_BotConversationHistory;
extern std::mutex   g_ConversationHistoryMutex;
extern time_t       g_LastHistorySaveTime;

// --------------------------------------------
// Blacklist: Prefixes for Commands (not chat)
// --------------------------------------------
extern std::vector<std::string> g_BlacklistCommands;

// --------------------------------------------
// Think Mode (see mod-ollama-chat_capability.h)
// --------------------------------------------
extern bool     g_ThinkModeEnableForModule;   // deprecated; maps onto ThinkMode
extern uint8_t  g_ThinkModePolicy;            // OllamaThinkPolicy
extern uint32_t g_ThinkMaxLatencyMs;
// Extra num_predict headroom granted when reasoning tokens are expected --
// either because think mode is on, or because the model reasons whether or not
// it is asked to. Without it a small NumPredict is spent on reasoning and the
// reply comes back empty. 0 disables the headroom entirely.
extern uint32_t g_ReasoningTokenReserve;
extern uint32_t g_CapabilityProbeTimeoutSeconds;

// --------------------------------------------
// HTTP / dispatcher
// --------------------------------------------
extern uint32_t g_HttpTimeoutSeconds;
extern uint32_t g_DispatchWorkerThreads;
extern uint32_t g_MaxQueueDepth;

// --------------------------------------------
// Response post-processing
// --------------------------------------------
extern uint32_t g_MaxReplyLength;
extern bool     g_ResponseStripMarkdown;
extern bool     g_ResponseStripDecorativeUnicode;

// --------------------------------------------
// Conversation governor
// --------------------------------------------
extern uint8_t  g_MaxChainDepth;
extern uint32_t g_ChainChanceDecayPct;
extern bool     g_RequireRecentHuman;
extern uint32_t g_HumanWindowSeconds;
extern uint32_t g_BotCooldownSeconds;
extern uint32_t g_ScopeCooldownSeconds;
extern uint32_t g_ScopeMessagesPerMinute;
extern uint32_t g_GlobalMessagesPerMinute;
extern uint32_t g_BotHistorySize;
extern uint32_t g_ScopeHistorySize;
extern float    g_RepetitionSimilarityThreshold;
extern uint32_t g_RepetitionWindowSeconds;
extern uint32_t g_OpenerHistorySize;

// --------------------------------------------
// Topic engine
// --------------------------------------------
extern uint32_t g_TopicWeightPeople;
extern uint32_t g_TopicWeightWorld;
extern uint32_t g_TopicWeightActivity;
extern uint32_t g_TopicWeightSelf;
extern uint32_t g_TopicWeightGuild;
extern uint32_t g_TopicMemoryCount;
extern uint32_t g_TopicEventMemorySize;
extern uint32_t g_TopicEventMemorySeconds;
extern float    g_TopicPlayerRadius;
extern uint32_t g_RandomChatterQuestionChance;

extern std::vector<std::string> g_EnvCommentNearbyPlayer;
extern std::vector<std::string> g_EnvCommentGroupMember;
extern std::vector<std::string> g_EnvCommentGuildMemberOnline;
extern std::vector<std::string> g_EnvCommentRecentEvent;
extern std::vector<std::string> g_EnvCommentNamedNpc;
extern std::vector<std::string> g_EnvCommentZoneLandmark;
extern std::vector<std::string> g_EnvCommentTimeOfDay;
extern std::vector<std::string> g_EnvCommentCorpse;
extern std::vector<std::string> g_EnvCommentDanger;
extern std::vector<std::string> g_EnvCommentQuestObjective;
extern std::vector<std::string> g_EnvCommentGroupNeed;

// --------------------------------------------
// Game-state snapshot limits
// --------------------------------------------
extern bool     g_SnapshotIncludeSpells;
extern uint32_t g_SnapshotMaxSpells;
extern uint32_t g_SnapshotMaxCreatures;
extern uint32_t g_SnapshotMaxObjects;
extern uint32_t g_SnapshotMaxPlayers;

// --------------------------------------------
// Roleplay mode
// --------------------------------------------
extern bool        g_RoleplayEnable;
extern uint8_t     g_RoleplayStrictness;
extern bool        g_RoleplayUseRaceVoice;
extern bool        g_RoleplayUseClassVoice;
extern bool        g_RoleplayFactionAttitude;
extern bool        g_RoleplayBlockMetaTerms;
extern std::string g_RoleplayMetaTermList;
extern bool        g_RoleplayCrossFactionGibberish;
extern std::vector<std::string> g_RoleplayPromptVariations;
extern std::vector<std::string> g_RoleplayQuestionVariations;

// --------------------------------------------
// How long a looping emote state (dance) is allowed to run before the module
// clears it. Nothing else clears it for a bot: the core only does so on a
// movement packet from a real client. 0 leaves it set forever.
extern uint32_t g_StateEmoteDurationMs;

// Percent chance (0-100) that a reply's prompt invites a gesture at all.
// Offering it on every line makes bots gesture almost constantly. 0 = never.
extern int      g_GestureChance;

// TEXT_EMOTE_* to play when the model supplies a gesture name the table does
// not know. Resolved from its config name on the world thread at load, because
// ExtractEmoteTag runs on a worker and must not read a config string. 0 = none.
extern uint32_t g_EmoteFallbackId;

// Embodiment: facing, gestures, emote reactions
// --------------------------------------------
extern bool     g_EnableBotFacing;
extern bool     g_EnableBotEmotes;
extern uint32_t g_BotExpressionDelayMs;
extern float    g_BotFacingMaxDistance;
extern bool     g_EnableEmoteReactions;
extern uint32_t g_EmoteReplyChance;
extern uint32_t g_EmoteReplyMirrorWeight;
extern uint32_t g_EmoteReplyCounterWeight;
extern uint32_t g_EmoteReplySpeakWeight;
extern uint32_t g_EmoteReactionCooldownSeconds;
extern std::string g_EmoteReactionPromptTemplate;

// --------------------------------------------
// Long-term memory and relationships
// --------------------------------------------
extern bool        g_MemoryEnable;
extern uint32_t    g_MemoryHistoryTokenLimit;   // condense once history exceeds this
extern uint32_t    g_MemoryPromptTokenBudget;   // how much of the prompt memories may use
extern uint32_t    g_MemoryMaxPerBot;
extern uint32_t    g_MemorySaveInterval;        // minutes
extern std::string g_MemoryCondensePrompt;
extern std::string g_MemoryPromptTemplate;

extern bool        g_RelationshipEnable;
extern uint32_t    g_RelationshipMentionThreshold;
extern uint32_t    g_RelationshipMaxPerPrompt;
extern uint32_t    g_RelationshipMaxLength;
extern std::string g_RelationshipUpdatePrompt;
extern std::string g_RelationshipPromptTemplate;

extern time_t      g_LastMemorySaveTime;

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
// (bot_guid, player_guid) pairs changed since the last save. Only these are
// written; the map as a whole is not re-REPLACE INTO'd every interval.
extern std::set<std::pair<uint64_t, uint64_t>> g_DirtySentiments;
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
// Channel Disable Settings
// --------------------------------------------
extern bool g_DisableForCustomChannels;
extern bool g_DisableForSayYell;
extern bool g_DisableForGuild;
extern bool g_DisableForParty;

// Which numbered channels ambient chatter may use.
extern bool g_ChatterUseGeneralChannel;
extern bool g_ChatterUseTradeChannel;
extern bool g_ChatterUseLfgChannel;
extern bool g_ChatterUseGuildRecruitmentChannel;

// --------------------------------------------
// Typing Simulation Settings
// --------------------------------------------
extern bool g_EnableTypingSimulation;
extern uint32_t g_TypingSimulationBaseDelay;      // Base delay in milliseconds
extern uint32_t g_TypingSimulationDelayPerChar;   // Delay per character in milliseconds
extern uint32_t g_TypingSimulationMaxDelay;       // Ceiling, so a long reply is not lost

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
