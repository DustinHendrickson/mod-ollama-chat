#ifndef MOD_OLLAMA_CHAT_RESPONSE_H
#define MOD_OLLAMA_CHAT_RESPONSE_H

#include <string>
#include <cstdint>

// --------------------------------------------------------------------------
// Response post-processing pipeline.
//
// Replaces the old ExtractTextBetweenDoubleQuotes(), which returned only the
// text between the first two double quotes anywhere in the reply and so
// destroyed any line that merely contained a quotation.
//
// The steps run in a fixed order; each is individually configurable. The
// pipeline never returns empty for input that had usable text in it -- a
// truncated <think> block is salvaged rather than discarded.
// --------------------------------------------------------------------------

// Strip a <think>...</think> block. Handles the unclosed case (model output
// was truncated mid-reasoning) by keeping whatever preceded the open tag
// instead of throwing the whole reply away.
std::string StripThinkTags(const std::string& text);

// Unwrap surrounding quotes only when the ENTIRE reply is wrapped in them.
std::string UnwrapQuotedReply(const std::string& text);

// Strip a leading speaker attribution the model added itself:
//   "Thrall: hey there"  /  "<Thrall> hey there"  /  "[Thrall] hey there"
std::string StripSpeakerPrefix(const std::string& text, const std::string& botName);

// Collapse CR/LF/tabs to single spaces and squeeze runs of whitespace.
std::string CollapseWhitespace(const std::string& text);

// Remove markdown emphasis, bullets, headings and code fences.
std::string StripMarkdown(const std::string& text);

// Fold smart punctuation to ASCII, then drop symbol/emoji codepoints that the
// 3.3.5 client cannot render. Latin-1 accented characters are preserved.
std::string StripDecorativeUnicode(const std::string& text);

// Clamp to maxLen bytes, cutting at the last sentence end if one is available
// in the tail, otherwise at the last word boundary. Never cuts mid-word and
// never splits a UTF-8 sequence.
std::string ClampReplyLength(const std::string& text, uint32_t maxLen);

// Run the full pipeline. Returns an empty string only when nothing usable
// survived, in which case the caller should skip the reply.
//
// outEmoteId, when non-null, receives the TEXT_EMOTE_* id parsed from an
// "[emote:name]" tag (0 when absent or unknown); the tag is removed from the
// text either way.
std::string ProcessLlmResponse(const std::string& raw,
                               const std::string& botName,
                               uint32_t* outEmoteId = nullptr);

#endif // MOD_OLLAMA_CHAT_RESPONSE_H
