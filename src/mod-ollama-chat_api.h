#ifndef MOD_OLLAMA_CHAT_API_H
#define MOD_OLLAMA_CHAT_API_H

#include <string>
#include <future>
#include "mod-ollama-chat_querymanager.h"

std::string QueryOllamaAPI(const std::string& prompt);

// Checks if an API response is valid (not an error message)
bool IsValidAPIResponse(const std::string& response);

// Submits a query to the API.
std::future<std::string> SubmitQuery(const std::string& prompt);

// Declare the global QueryManager variable.
extern QueryManager g_queryManager;

#endif // MOD_OLLAMA_CHAT_API_H
