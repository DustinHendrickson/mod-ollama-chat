#include "mod-ollama-chat_buffs.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_expression.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_world.h"

#include "CellImpl.h"
#include "Config.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <vector>

namespace
{
    struct BuffConfig
    {
        bool     enable          = true;
        float    range           = 25.0f;
        uint32_t botsPerSecond   = 15;
        uint32_t minInterval     = 8;     // seconds between one bot's buffs
        uint32_t maxInterval     = 20;
        float    minSentiment    = 0.3f;
        bool     buffBots        = true;
        uint32_t minManaPct      = 40;
        uint32_t emoteChance     = 25;    // percent
        uint32_t retrySeconds    = 300;   // after a cast that would not go off on someone
    };
    BuffConfig g_bc;

    // A bot's buff spells, highest rank known, rebuilt now and then (it
    // learns new ranks as it levels).
    struct BotBuffs
    {
        std::vector<uint32_t> spells;
        uint32_t              builtAt = 0;
        uint32_t              nextAt  = 0;   // when it may buff someone again
    };

    std::unordered_map<uint64_t, BotBuffs>  g_bots;
    std::unordered_map<uint64_t, uint32_t>  g_refused;   // (bot ^ target) -> retry after
    std::vector<uint64_t>                   g_roster;
    size_t                                  g_cursor       = 0;
    uint32_t                                g_tickTimer    = 0;
    uint32_t                                g_rosterTimer  = 0;

    uint32_t Now() { return uint32_t(std::time(nullptr)); }

    bool IsBuffAura(AuraType type)
    {
        switch (type)
        {
            case SPELL_AURA_MOD_STAT:
            case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
            case SPELL_AURA_MOD_RESISTANCE:
            case SPELL_AURA_MOD_RESISTANCE_EXCLUSIVE:
            case SPELL_AURA_MOD_BASE_RESISTANCE:
            case SPELL_AURA_MOD_ATTACK_POWER:
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_MOD_POWER_REGEN_PERCENT:
            case SPELL_AURA_MOD_INCREASE_HEALTH:
            case SPELL_AURA_DAMAGE_SHIELD:   // Thorns
                return true;
            default:
                return false;
        }
    }

    // An instant, long, single-target buff for an ally.
    bool IsPasserByBuff(SpellInfo const* info, Player* caster)
    {
        if (!info || info->IsPassive() || !info->IsPositive() || info->IsChanneled())
            return false;
        if (info->GetMaxDuration() < int32(10 * MINUTE * IN_MILLISECONDS))
            return false;
        if (info->CalcCastTime(caster) > 0)
            return false;
        for (int32 reagent : info->Reagent)
            if (reagent > 0)
                return false;

        bool any = false;
        for (SpellEffectInfo const& e : info->GetEffects())
        {
            if (!e.Effect)
                continue;
            if (e.Effect != SPELL_EFFECT_APPLY_AURA || e.TargetA.GetTarget() != TARGET_UNIT_TARGET_ALLY ||
                !IsBuffAura(e.ApplyAuraName))
                return false;
            any = true;
        }
        return any;
    }

    std::vector<uint32_t> FindBuffs(Player* bot)
    {
        std::vector<uint32_t> out;
        for (auto const& [spellId, spell] : bot->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active ||
                !spell->IsInSpec(bot->GetActiveSpec()))
                continue;
            // The highest rank known only.
            if (uint32 next = sSpellMgr->GetNextSpellInChain(spellId); next && bot->HasSpell(next))
                continue;
            if (IsPasserByBuff(sSpellMgr->GetSpellInfo(spellId), bot))
                out.push_back(spellId);
        }
        return out;
    }

    // Any rank of this buff already on them, from anyone.
    bool HasAnyRank(Unit* target, uint32_t spellId)
    {
        for (uint32 id = sSpellMgr->GetFirstSpellInChain(spellId); id; id = sSpellMgr->GetNextSpellInChain(id))
            if (target->HasAura(id))
                return true;
        return target->HasAura(spellId);   // a spell with no chain
    }

    // A paladin's blessings replace each other: one per paladin per person.
    bool HasBlessingFrom(Unit* target, Player* paladin)
    {
        for (auto const& [id, app] : target->GetAppliedAuras())
        {
            Aura const* aura = app ? app->GetBase() : nullptr;
            if (aura && aura->GetCasterGUID() == paladin->GetGUID() &&
                aura->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_PALADIN &&
                aura->GetSpellInfo()->GetMaxDuration() >= int32(10 * MINUTE * IN_MILLISECONDS))
                return true;
        }
        return false;
    }

    bool UsesMana(Player* p)
    {
        return p->getPowerType() == POWER_MANA;
    }

    // Kings for anyone; otherwise Wisdom for a mana user, Might for the rest.
    int BlessingRank(SpellInfo const* info, Player* target)
    {
        for (SpellEffectInfo const& e : info->GetEffects())
        {
            if (e.ApplyAuraName == SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE)
                return 0;
            if (e.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN)
                return UsesMana(target) ? 1 : 3;
            if (e.ApplyAuraName == SPELL_AURA_MOD_ATTACK_POWER)
                return UsesMana(target) ? 3 : 1;
        }
        return 2;
    }

    uint64_t PairKey(uint64_t bot, uint64_t target) { return bot * 1000003ULL ^ target; }

    bool CanBuffNow(Player* bot)
    {
        if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() ||
            bot->IsSitState() || bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_CASTING) ||
            bot->HasStealthAura() || bot->HasInvisibilityAura() || bot->InBattleground() || bot->InArena())
            return false;
        if (bot->GetMaxPower(POWER_MANA) > 0 &&
            bot->GetPower(POWER_MANA) * 100 < bot->GetMaxPower(POWER_MANA) * g_bc.minManaPct)
            return false;
        return true;
    }

    bool IsCandidate(Player* bot, Player* target)
    {
        if (target == bot || !target->IsInWorld() || !target->IsAlive() || target->IsInCombat() ||
            target->IsInFlight() || target->IsGameMaster() || target->duel || !bot->IsValidAssistTarget(target) ||
            !bot->IsFriendlyTo(target) || !bot->CanSeeOrDetect(target))
            return false;
        // Buffing a flagged player would flag the bot; a stranger would not.
        if (target->IsPvP() && !bot->IsPvP())
            return false;
        const bool real = OllamaIsRealPlayer(target);
        if (!real && !g_bc.buffBots)
            return false;
        if (GetBotPlayerSentiment(bot->GetGUID().GetRawValue(), target->GetGUID().GetRawValue()) < g_bc.minSentiment)
            return false;
        return bot->IsWithinLOSInMap(target);
    }

    // One buff, at most, for one passer-by. True when a spell went off.
    bool TryBuff(Player* bot, PlayerbotAI* ai, BotBuffs& mine, uint32_t now)
    {
        std::vector<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(bot, g_bc.range, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, nearby, check);
        Cell::VisitObjects(bot, searcher, g_bc.range);

        // Real people first, then the nearest.
        std::sort(nearby.begin(), nearby.end(), [bot](Player* a, Player* b)
        {
            const bool ra = OllamaIsRealPlayer(a), rb = OllamaIsRealPlayer(b);
            if (ra != rb)
                return ra;
            return bot->GetExactDist2dSq(a) < bot->GetExactDist2dSq(b);
        });

        const uint64_t botGuid = bot->GetGUID().GetRawValue();
        const bool     paladin = bot->getClass() == CLASS_PALADIN;
        for (Player* target : nearby)
        {
            if (!IsCandidate(bot, target))
                continue;
            const uint64_t key = PairKey(botGuid, target->GetGUID().GetRawValue());
            if (auto r = g_refused.find(key); r != g_refused.end() && now < r->second)
                continue;
            if (paladin && HasBlessingFrom(target, bot))
                continue;

            std::vector<uint32_t> order = mine.spells;
            if (paladin)
                std::stable_sort(order.begin(), order.end(), [target](uint32_t a, uint32_t b)
                {
                    return BlessingRank(sSpellMgr->GetSpellInfo(a), target) <
                           BlessingRank(sSpellMgr->GetSpellInfo(b), target);
                });

            for (uint32_t spell : order)
            {
                if (HasAnyRank(target, spell) || !ai->CanCastSpell(spell, target, true))
                    continue;
                if (ai->CastSpell(spell, target))
                {
                    if (urand(1, 100) <= g_bc.emoteChance)
                        BotPlayTextEmote(bot, TEXT_EMOTE_WAVE, target->GetGUID());
                    if (g_DebugEnabled)
                        LOG_INFO("module.ollamachat", "[Ollama Chat] {} buffed passer-by {} with spell {}.",
                                 bot->GetName(), target->GetName(), spell);
                    return true;
                }
                // A stronger buff on them, out of range, ...: leave them be a while.
                g_refused[key] = now + g_bc.retrySeconds;
                break;
            }
            if (paladin)
                continue;   // a blessing that would not go off is not a reason to stop looking
        }
        return false;
    }

    void RebuildRoster()
    {
        g_roster.clear();
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player && OllamaIsBotPlayer(player))
                g_roster.push_back(player->GetGUID().GetRawValue());
        if (g_cursor >= g_roster.size())
            g_cursor = 0;

        // Forget bots that went away, and stale refusals.
        const uint32_t now = Now();
        for (auto it = g_bots.begin(); it != g_bots.end();)
            it = std::find(g_roster.begin(), g_roster.end(), it->first) == g_roster.end() ? g_bots.erase(it)
                                                                                          : std::next(it);
        for (auto it = g_refused.begin(); it != g_refused.end();)
            it = now >= it->second ? g_refused.erase(it) : std::next(it);
    }
}

void Buffs_LoadConfig()
{
    BuffConfig c;
    c.enable        = sConfigMgr->GetOption<bool>("OllamaChat.Buff.Enable", true);
    c.range         = std::clamp(sConfigMgr->GetOption<float>("OllamaChat.Buff.Range", 25.0f), 5.0f, 40.0f);
    c.botsPerSecond = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("OllamaChat.Buff.BotsPerSecond", 15));
    c.minInterval   = sConfigMgr->GetOption<uint32_t>("OllamaChat.Buff.MinIntervalSeconds", 8);
    c.maxInterval   = std::max(c.minInterval, sConfigMgr->GetOption<uint32_t>("OllamaChat.Buff.MaxIntervalSeconds", 20));
    c.minSentiment  = sConfigMgr->GetOption<float>("OllamaChat.Buff.MinSentiment", 0.3f);
    c.buffBots      = sConfigMgr->GetOption<bool>("OllamaChat.Buff.BuffBots", true);
    c.minManaPct    = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Buff.MinManaPct", 40));
    c.emoteChance   = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("OllamaChat.Buff.WaveChance", 25));
    g_bc = c;
    g_bots.clear();   // spell lists are rebuilt with the new settings
}

void Buffs_Update(uint32_t diff)
{
    if (!g_bc.enable)
        return;

    g_rosterTimer += diff;
    if (g_rosterTimer >= 10000 || g_roster.empty())
    {
        g_rosterTimer = 0;
        RebuildRoster();
    }

    g_tickTimer += diff;
    if (g_tickTimer < 1000)
        return;
    g_tickTimer = 0;

    const uint32_t now   = Now();
    const size_t   limit = std::min<size_t>(g_bc.botsPerSecond, g_roster.size());
    for (size_t i = 0; i < limit && !g_roster.empty(); ++i)
    {
        if (g_cursor >= g_roster.size())
            g_cursor = 0;
        const uint64_t guid = g_roster[g_cursor++];

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!ai)
            continue;

        BotBuffs& mine = g_bots[guid];
        if (now < mine.nextAt)
            continue;
        if (!mine.builtAt || now - mine.builtAt >= 600)
        {
            mine.spells  = FindBuffs(bot);
            mine.builtAt = now;
        }
        if (mine.spells.empty() || !CanBuffNow(bot))
            continue;

        if (TryBuff(bot, ai, mine, now))
            mine.nextAt = now + urand(g_bc.minInterval, g_bc.maxInterval);
        else
            mine.nextAt = now + 3;   // nobody to buff: look again shortly
    }
}
