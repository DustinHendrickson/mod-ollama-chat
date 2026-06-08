-- One-time wipe of accumulated bot<->player conversation history.
--
-- The old history was generated under the previous (rambly, repetitive) prompts
-- and personalities; carrying it forward would feed those patterns back into the
-- new replies. This clears the slate so history starts fresh under the new setup.
--
-- This runs once on the next worldserver startup (tracked in `updates`), then
-- history accumulates normally again. A fresh process starts with empty in-memory
-- history, so after this DELETE there is nothing left to reload.

DELETE FROM `mod_ollama_chat_history`;
