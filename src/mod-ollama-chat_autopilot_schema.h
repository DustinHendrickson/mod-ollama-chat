#ifndef MOD_OLLAMA_CHAT_AUTOPILOT_SCHEMA_H
#define MOD_OLLAMA_CHAT_AUTOPILOT_SCHEMA_H

#include <string>

// --------------------------------------------------------------------------
// The autopilot tables, brought to the current layout at startup.
//
// data/sql/characters/base/2026_10_05_autopilot.sql creates them, and the
// core's updater applies it -- but only once per content change, and only as
// CREATE TABLE IF NOT EXISTS, so a table made by an earlier draft of this
// feature was never brought up to date. Rather than ask operators to drop
// tables by hand, this checks every table and column against the layout
// below and repairs it: missing tables are created, missing columns added,
// columns from earlier drafts dropped. Every change is logged.
//
// The layout here and the SQL file must describe the same tables.
//
// World thread, at startup (synchronous queries).
// --------------------------------------------------------------------------

// True when all three tables match the current layout afterwards. On false,
// `problem` says what could not be fixed.
bool AutopilotSchema_Ensure(std::string& problem);

#endif // MOD_OLLAMA_CHAT_AUTOPILOT_SCHEMA_H
