#ifndef MOD_OLLAMA_CHAT_BUFFS_H
#define MOD_OLLAMA_CHAT_BUFFS_H

#include <cstdint>

// --------------------------------------------------------------------------
// Passer-by buffs: the old-world courtesy of a stranger's Fortitude, Mark of
// the Wild or Arcane Intellect as you run past. Bots out of combat buff
// friendly players (and, if enabled, other bots) near them with the buffs
// their own class knows, unless they dislike the person (sentiment below
// OllamaChat.Buff.MinSentiment).
//
// Buffs come from each bot's own spellbook: instant, long, single-target ally
// auras that raise stats, armour, resistances, attack power, regeneration or
// health, or Thorns. Self-only spells, group versions (Prayer of..., Gift of
// the Wild, Greater Blessings) and situational ones are left out.
//
// World thread only: runs from the module's world tick, after the maps.
// --------------------------------------------------------------------------

void Buffs_LoadConfig();
void Buffs_Update(uint32_t diff);

#endif // MOD_OLLAMA_CHAT_BUFFS_H
