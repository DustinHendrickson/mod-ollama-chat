#include "mod-ollama-chat_autopilot_group.h"
#include "mod-ollama-chat-utilities.h"

#include "CharacterCache.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <cctype>

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    bool StartsWithWord(const std::string& text, const std::string& word)
    {
        return text == word || (text.size() > word.size() && text.compare(0, word.size(), word) == 0 &&
                                text[word.size()] == ' ');
    }

    // The first word and the rest, with the original case kept.
    std::string TakeWord(std::string& rest)
    {
        rest = Trim(rest);
        const size_t space = rest.find(' ');
        std::string word = rest.substr(0, space);
        rest = space == std::string::npos ? std::string() : Trim(rest.substr(space + 1));
        return word;
    }

    // An online player by name, as the client's name box takes it.
    Player* FindByName(std::string name)
    {
        if (!normalizePlayerName(name))
            return nullptr;
        return ObjectAccessor::FindPlayerByName(name, true);
    }

    // Who someone is, as a player in this world can tell: the other faction
    // cannot be whispered or invited.
    std::string CheckReachable(Player* bot, Player* target, const std::string& name)
    {
        if (!target)
            return name + " is not online";
        if (target == bot)
            return "that is themselves";
        if (target->GetTeamId() != bot->GetTeamId())
            return name + " is of the other faction";
        return "";
    }

    std::string Invite(Player* bot, std::string rest)
    {
        const std::string name = TakeWord(rest);
        if (name.empty())
            return "say who: group invite <name>";
        Player* target = FindByName(name);
        if (const std::string why = CheckReachable(bot, target, name); !why.empty())
            return why;

        Group* group = bot->GetGroup();
        if (group && group->GetLeaderGUID() != bot->GetGUID())
            return "only their group's leader can invite";
        if (group && group->IsFull())
            return "their group is full";
        if (target->GetGroup())
            return target->GetName() + " is already in a group";
        if (target->GetGroupInvite())
            return target->GetName() + " already has an invite waiting";

        WorldPacket packet(CMSG_GROUP_INVITE, target->GetName().size() + 5);
        packet << target->GetName();
        packet << uint32(0);
        bot->GetSession()->HandleGroupInviteOpcode(packet);

        Group* invitedTo = target->GetGroupInvite();
        if (!invitedTo || (bot->GetGroup() && invitedTo != bot->GetGroup()) ||
            (!bot->GetGroup() && invitedTo->GetLeaderGUID() != bot->GetGUID()))
            return "the invite did not go through (they may be ignoring them, or busy)";
        return "done: invited " + target->GetName() + "; waiting for their answer";
    }

    std::string Accept(Player* bot, PlayerbotAI* ai)
    {
        Group* invite = bot->GetGroupInvite();
        if (!invite)
            return "no group invite is waiting";
        Player* inviter = ObjectAccessor::FindPlayer(invite->GetLeaderGUID());
        if (!inviter)
            return "whoever invited them is gone";

        WorldPacket packet(CMSG_GROUP_ACCEPT, 4);
        packet << uint32(0);   // roles
        bot->GetSession()->HandleGroupAcceptOpcode(packet);
        if (!bot->GetGroup() || !bot->GetGroup()->IsMember(inviter->GetGUID()))
            return "could not join (the group may be full or gone)";

        // As playerbots' own accept does, without its summon: a random bot's
        // master is its leader, whom its follow strategy follows.
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
            ai->SetMaster(inviter);
        ai->ResetStrategies();
        ai->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
        ai->Reset();
        return "done: joined " + inviter->GetName() + "'s group";
    }

    std::string Decline(Player* bot)
    {
        Group* invite = bot->GetGroupInvite();
        if (!invite)
            return "no group invite is waiting";
        Player* inviter = ObjectAccessor::FindPlayer(invite->GetLeaderGUID());
        WorldPacket packet(CMSG_GROUP_DECLINE, 0);
        bot->GetSession()->HandleGroupDeclineOpcode(packet);
        return "done: declined" + (inviter ? " " + inviter->GetName() + "'s invite" : std::string(" the invite"));
    }

    std::string Leave(Player* bot, PlayerbotAI* ai)
    {
        if (!bot->GetGroup())
            return "not in a group";
        WorldPacket packet(CMSG_GROUP_DISBAND, 0);   // the client's Leave Party
        bot->GetSession()->HandleGroupDisbandOpcode(packet);
        if (bot->GetGroup())
            return "could not leave the group";

        // As playerbots' own leave does: no master, defaults for a bot alone.
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
            ai->SetMaster(nullptr);
        ai->ResetStrategies();
        ai->Reset();
        return "done: left the group";
    }

    std::string Share(Player* bot, std::string rest)
    {
        if (!bot->GetGroup())
            return "not in a group";
        const std::string idText = TakeWord(rest);
        uint32_t questId = 0;
        try { questId = static_cast<uint32_t>(std::stoul(idText)); }
        catch (...) { return "give the quest's id: group share <id>"; }

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || bot->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE)
            return "that quest is not in their log";
        if (!bot->CanShareQuest(questId))
            return quest->GetTitle() + " cannot be shared";

        WorldPacket raw(CMSG_PUSHQUESTTOPARTY, 4);
        raw << questId;
        WorldPackets::Quest::PushQuestToParty packet(std::move(raw));
        packet.Read();
        bot->GetSession()->HandlePushQuestToParty(packet);
        return "done: offered " + quest->GetTitle() + " to the group";
    }

    std::string Whisper(Player* bot, std::string rest, std::string& whisperedTo)
    {
        const std::string name = TakeWord(rest);
        if (name.empty() || rest.empty())
            return "use whisper <name> <message>";
        Player* target = FindByName(name);
        if (const std::string why = CheckReachable(bot, target, name); !why.empty())
            return why;
        if (rest.size() > 240)
            rest.resize(240);
        bot->Whisper(rest, LANG_UNIVERSAL, target);
        whisperedTo = target->GetName();
        return "done: whispered " + target->GetName();
    }
}

bool AutopilotGroup_IsOrder(const std::string& command)
{
    const std::string c = Lower(Trim(command));
    return StartsWithWord(c, "group") || StartsWithWord(c, "invite") || StartsWithWord(c, "whisper");
}

bool AutopilotGroup_ChangesMembership(const std::string& command)
{
    const std::string c = Lower(Trim(command));
    return StartsWithWord(c, "invite") ||
           (StartsWithWord(c, "group") && !StartsWithWord(Trim(c.substr(5)), "share"));
}

std::string AutopilotGroup_Run(Player* bot, PlayerbotAI* ai, const std::string& command, std::string& whisperedTo)
{
    if (!bot || !ai || !bot->GetSession())
        return "not now";

    std::string rest = Trim(command);
    const std::string verb = Lower(TakeWord(rest));

    if (verb == "whisper")
        return Whisper(bot, rest, whisperedTo);
    if (verb == "invite")
        return Invite(bot, rest);
    if (verb != "group")
        return "unknown order";

    const std::string what = Lower(TakeWord(rest));
    if (what == "invite")
        return Invite(bot, rest);
    if (what == "accept" || what == "join")
        return Accept(bot, ai);
    if (what == "decline")
        return Decline(bot);
    if (what == "leave")
        return Leave(bot, ai);
    if (what == "share")
        return Share(bot, rest);
    return "use group invite <name>, group accept, group decline, group leave, or group share <quest id>";
}

Player* AutopilotGroup_Inviter(Player* bot)
{
    Group* invite = bot ? bot->GetGroupInvite() : nullptr;
    return invite ? ObjectAccessor::FindPlayer(invite->GetLeaderGUID()) : nullptr;
}

float AutopilotGroup_Straggler(Player* leader, float minDist, float maxDist)
{
    Group* group = leader ? leader->GetGroup() : nullptr;
    if (!group)
        return 0.0f;
    float worst = 0.0f;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == leader || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMap() != leader->GetMap() || member->IsInFlight())
            continue;
        const float d = leader->GetDistance(member);
        if (d > minDist && d <= maxDist)
            worst = std::max(worst, d);
    }
    return worst;
}
