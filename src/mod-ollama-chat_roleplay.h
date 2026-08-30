#ifndef MOD_OLLAMA_CHAT_ROLEPLAY_H
#define MOD_OLLAMA_CHAT_ROLEPLAY_H

#include <string>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Roleplay mode.
//
// The old EnableRPPersonalities only swapped which personality string got
// appended. Race and class were passed as bare facts -- "Tauren", "Druid" --
// with no voice attached, while the shipped chatter variations actively pushed
// the other way ("Complain about nerfed abilities", "Mention Death Knights
// being OP"). That is player-forum talk, not in-world talk.
//
// This adds two data-driven voice packs and a strictness dial:
//
//   race voice  -- speech register, cultural touchstones, what they call the
//                  other faction.
//   class voice -- worldview: what this character NOTICES about a scene.
//
// Strictness 0 = flavour only, 1 = in character with meta terms filtered,
// 2 = hard in character with mechanical numbers described rather than quoted.
// --------------------------------------------------------------------------

void Roleplay_Load();     // seed defaults from conf, then overlay DB rows

// The prompt fragment describing how this character speaks and what they
// notice. Empty when roleplay mode is off.
std::string Roleplay_BuildVoicePrompt(Player* bot);

// Strip out-of-world vocabulary ("dps", "nerf", "proc", "respec", ...).
// Applied at strictness >= 1.
std::string Roleplay_FilterMetaTerms(const std::string& text);

// Replace exact figures with description: "340/450" -> "badly wounded".
// Applied at strictness >= 2 so bots stop reading their own health bar aloud.
std::string Roleplay_DescribeHealth(uint32_t current, uint32_t max);

// True when the two players could not understand each other in-game and
// roleplay mode is configured to respect that.
bool Roleplay_IsLanguageBarrier(Player* speaker, Player* listener);

// Which chatter variation lists to use. At strictness 2 the roleplay lists
// replace the shipped out-of-character ones.
bool Roleplay_UseRoleplayVariations();

#endif // MOD_OLLAMA_CHAT_ROLEPLAY_H
