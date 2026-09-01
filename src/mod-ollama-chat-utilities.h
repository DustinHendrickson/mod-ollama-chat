#ifndef MOD_OLLAMA_CHAT_UTILS_H
#define MOD_OLLAMA_CHAT_UTILS_H

#include <string>
#include <fmt/format.h>
#include "Log.h"
#include <vector>
#include <sstream>
#include <cctype>

// Safe formatting utility for the Ollama Chat module.
// This will catch all fmt::format errors and log them.
template<typename... Args>
inline std::string SafeFormat(const std::string& templ, Args&&... args) {
    try {
        return fmt::vformat(templ, fmt::make_format_args(args...));
    } catch (const fmt::format_error& e) {
        LOG_ERROR("server.loading", "[Ollama Chat] Format error: {} | Template: {}", e.what(), templ);
        return "[Format Error]";
    }
}

// ChatHelper::FormatClass() builds a string for the WoW chat window, not for a
// prompt. It comes back as
//
//     holy (0|h|cffffffff/0|h|cffffffff/0|h|cffffffff)|r priest
//
// The escape codes are the client's colour and hyperlink markup; a model just
// sees noise, and it is repeated for both speakers in every prompt. The talent
// counts go too -- "(0/0/0)" tells a roleplaying bot nothing.
//
// Leaves "holy priest".
inline std::string CleanRoleForPrompt(const std::string& role)
{
    std::string out;
    out.reserve(role.size());

    // Strip |cAARRGGBB, |r, |h and |H.
    for (size_t i = 0; i < role.size(); )
    {
        if (role[i] == '|' && i + 1 < role.size())
        {
            const char code = role[i + 1];

            if (code == 'c')
            {
                i += 2;
                for (size_t n = 0; n < 8 && i < role.size() &&
                                   std::isxdigit(static_cast<unsigned char>(role[i])); ++n)
                    ++i;
                continue;
            }

            if (code == 'r' || code == 'h' || code == 'H')
            {
                i += 2;
                continue;
            }
        }

        out += role[i++];
    }

    // Drop a parenthesised talent spread, but only when that is all it holds.
    const size_t open = out.find('(');
    if (open != std::string::npos)
    {
        const size_t close = out.find(')', open);
        if (close != std::string::npos && close > open + 1)
        {
            bool numeric = true;
            for (size_t i = open + 1; i < close && numeric; ++i)
            {
                const char c = out[i];
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '/' && c != ' ')
                    numeric = false;
            }

            if (numeric)
                out.erase(open, close - open + 1);
        }
    }

    // Collapse the whitespace the removals left behind.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prevSpace = false;
    for (char c : out)
    {
        const bool isSpace = (c == ' ' || c == '	');
        if (isSpace && (prevSpace || collapsed.empty()))
            continue;

        collapsed += isSpace ? ' ' : c;
        prevSpace = isSpace;
    }

    while (!collapsed.empty() && collapsed.back() == ' ')
        collapsed.pop_back();

    return collapsed;
}

inline std::vector<std::string> SplitString(const std::string& str, char delim)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim))
    {
        // Trim whitespace from token
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos)
            tokens.push_back(token.substr(start, end - start + 1));
    }
    return tokens;
}

// Sanitize a string to be valid UTF-8 by removing or replacing invalid bytes
inline std::string SanitizeUTF8(const std::string& str)
{
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < str.size(); )
    {
        unsigned char c = static_cast<unsigned char>(str[i]);
        
        // Single-byte character (0xxxxxxx)
        if (c <= 0x7F)
        {
            result.push_back(str[i]);
            i++;
        }
        // Two-byte character (110xxxxx 10xxxxxx)
        else if ((c & 0xE0) == 0xC0)
        {
            if (i + 1 < str.size() && (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                i += 2;
            }
            else
            {
                // Invalid sequence, replace with space
                result.push_back(' ');
                i++;
            }
        }
        // Three-byte character (1110xxxx 10xxxxxx 10xxxxxx)
        else if ((c & 0xF0) == 0xE0)
        {
            if (i + 2 < str.size() &&
                (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 2]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                result.push_back(str[i + 2]);
                i += 3;
            }
            else
            {
                // Invalid sequence, replace with space
                result.push_back(' ');
                i++;
            }
        }
        // Four-byte character (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
        else if ((c & 0xF8) == 0xF0)
        {
            if (i + 3 < str.size() &&
                (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 3]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                result.push_back(str[i + 2]);
                result.push_back(str[i + 3]);
                i += 4;
            }
            else
            {
                // Invalid sequence, replace with space
                result.push_back(' ');
                i++;
            }
        }
        else
        {
            // Invalid UTF-8 start byte, replace with space
            result.push_back(' ');
            i++;
        }
    }
    
    return result;
}

#endif // MOD_OLLAMA_CHAT_UTILS_H
