#ifndef MOD_OLLAMA_CHAT_PROGRESS_H
#define MOD_OLLAMA_CHAT_PROGRESS_H

#include <cstdint>
#include <string>
#include <vector>

class Field;
class Player;

// --------------------------------------------------------------------------
// Autopilot progress history: the bot's diary.
//
// Two kinds of record, both written in batches by Progress_Flush():
//
//   snapshot  a periodic sample of the bot's macro state. Counters are running
//             totals, so thinning old rows loses resolution but never counts.
//   event     a notable moment (level, death, quest, rare loot, zone change).
//
// Writes are queued in memory and flushed in one async transaction. History
// is thinned as it ages: every snapshot for a day, one per hour for a week,
// one per day after that, plus a hard cap per bot. Events keep the newest N.
//
// Snapshots are captured from Player fields only -- no grid scans -- so a
// capture is cheap enough to run for hundreds of bots on a staggered timer.
// --------------------------------------------------------------------------

struct ProgressSnapshot
{
    uint64_t    botGuid        = 0;
    uint32_t    takenAt        = 0;   // unix seconds
    uint8_t     level          = 0;
    uint32_t    xp             = 0;
    uint32_t    money          = 0;   // copper
    uint16_t    mapId          = 0;
    uint32_t    zoneId         = 0;
    uint32_t    areaId         = 0;
    uint8_t     durabilityPct  = 100;
    uint16_t    freeBagSlots   = 0;
    uint8_t     questsActive   = 0;
    uint32_t    questsRewarded = 0;   // lifetime, from the character
    uint32_t    killsTotal     = 0;   // since enrollment
    uint32_t    deathsTotal    = 0;   // since enrollment
    uint32_t    questsTotal    = 0;   // completions since enrollment
    std::string professions;          // "skillId:value/max,..."
};

struct ProgressEvent
{
    uint64_t    botGuid = 0;
    uint32_t    at      = 0;
    std::string type;
    std::string detail;
};

// Read the bot's own fields. Call where that Player is safe to read: the world
// thread, or the bot's own map update. The *_Total counters are left at 0 for
// the caller to fill.
ProgressSnapshot Progress_Capture(Player* bot);

// Equipped-gear durability, 0-100. Same thread rule as Progress_Capture.
uint8_t Progress_DurabilityPct(Player* bot);

// The snapshot column list for a SELECT, and a reader for one row of it.
// Shared by the synchronous reads below and autopilot's async history load.
const char*      Progress_SnapshotColumns();
ProgressSnapshot Progress_ReadSnapshot(Field* fields, uint64_t botGuid);

// Queue for the next flush. Thread-safe.
void Progress_QueueSnapshot(ProgressSnapshot snapshot);
void Progress_QueueEvent(uint64_t botGuid, std::string type, std::string detail);

// Drop anything still queued for a character that no longer exists, so a
// later flush does not write orphan rows for it. Thread-safe.
void Progress_Forget(uint64_t botGuid);

// Write everything queued, then thin history for bots due a trim.
// World thread.
void Progress_Flush(uint32_t maxSnapshotsPerBot, uint32_t maxEventsPerBot);

// --- reads (synchronous; for GM commands, not for per-tick use) -----------

// Newest first, including events still waiting for a flush.
std::vector<ProgressEvent> Progress_LoadEvents(uint64_t botGuid, uint32_t limit);

// Oldest first, taken at or after `sinceUnix`.
std::vector<ProgressSnapshot> Progress_LoadSnapshots(uint64_t botGuid, uint32_t sinceUnix,
                                                     uint32_t limit);

// "Mining 75/150, Herbalism 60/75" from the compact column.
std::string Progress_DescribeProfessions(const std::string& compact);

// Prompt text describing change across a window of snapshots (oldest first):
// "Over the last 3h: level 21 -> 23, +12g 40s 0c, 9 quests done, 140 kills,
// 2 deaths. Zones: Westfall, Duskwood. Mining 75 -> 98." Deltas, not raw
// rows: what changed is what the model should reason about.
std::string Progress_Summarize(const std::vector<ProgressSnapshot>& oldestFirst);

std::string Progress_ZoneName(uint32_t zoneId);

// Primary and secondary profession skill ids, and a skill's display name.
const std::vector<uint32_t>& Progress_ProfessionSkills();
std::string                  Progress_SkillName(uint32_t skillId);

// "12g 34s 5c"
std::string Progress_FormatMoney(int64_t copper);

uint32_t Progress_Now();

#endif // MOD_OLLAMA_CHAT_PROGRESS_H
