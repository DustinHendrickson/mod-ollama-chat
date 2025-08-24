#ifndef MOD_OLLAMA_CHAT_UTILS_H
#define MOD_OLLAMA_CHAT_UTILS_H

#include <string>
#include <fmt/core.h>
#include "Log.h"
#include <vector>
#include <sstream>

// Safe formatting utility for the Ollama Chat module.
// This will catch all fmt::format errors and log them.
template<typename... Args>
inline std::string SafeFormat(const std::string& templ, Args&&... args) {
    try {
        return fmt::format(templ, std::forward<Args>(args)...);
    } catch (const fmt::format_error& e) {
        LOG_ERROR("server.loading", "[Ollama Chat] Format error: {} | Template: {}", e.what(), templ);
        return "[Format Error]";
    }
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

#endif // MOD_OLLAMA_CHAT_UTILS_H
