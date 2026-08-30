#ifndef MOD_OLLAMA_CHAT_RANDOM_H
#define MOD_OLLAMA_CHAT_RANDOM_H

#include "ScriptMgr.h"
#include "ObjectGuid.h"

// Drives ambient bot chatter, and hosts the module's world tick: the
// dispatcher, governor and topic memory are all pumped from here.
class OllamaBotRandomChatter : public WorldScript
{
public:
    OllamaBotRandomChatter();
    void OnUpdate(uint32 diff) override;

private:
    void HandleRandomChatter();
};

// Drops a bot's random-chatter schedule on logout. The old map keyed every
// bot GUID the server had ever seen and never released one.
void OllamaRandomChatter_ForgetBot(ObjectGuid botGuid);

#endif // MOD_OLLAMA_CHAT_RANDOM_H
