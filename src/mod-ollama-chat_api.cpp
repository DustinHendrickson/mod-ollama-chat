#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "Log.h"
#include <curl/curl.h>
#include <sstream>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <thread>
#include <mutex>
#include <queue>
#include <future>

// Callback for cURL write function.
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    std::string* responseBuffer = static_cast<std::string*>(userp);
    size_t totalSize = size * nmemb;
    responseBuffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string ExtractTextBetweenDoubleQuotes(const std::string& response)
{
    size_t first = response.find('\"');
    size_t second = response.find('\"', first + 1);
    if (first != std::string::npos && second != std::string::npos) {
        return response.substr(first + 1, second - first - 1);
    }
    return response;
}

// Function to perform the API call.
std::string QueryOllamaAPI(const std::string& prompt)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Failed to initialize cURL.");
        }
        return "Hmm... I'm lost in thought.";
    }

    std::string url = g_OllamaUrl;
    std::string model = g_OllamaModel;

    nlohmann::json requestData = {
        {"model",  model},
        {"prompt", prompt}
    };
    // Only include if set (do not send defaults if user did not set them)
    if (g_OllamaNumPredict > 0)          requestData["num_predict"] = g_OllamaNumPredict;
    if (g_OllamaTemperature != 0.8f)     requestData["temperature"] = g_OllamaTemperature;
    if (g_OllamaTopP != 0.95f)           requestData["top_p"] = g_OllamaTopP;
    if (g_OllamaRepeatPenalty != 1.1f)   requestData["repeat_penalty"] = g_OllamaRepeatPenalty;
    if (g_OllamaNumCtx > 0)              requestData["num_ctx"] = g_OllamaNumCtx;
    if (!g_OllamaStop.empty()) {
        // If comma-separated, convert to array
        std::vector<std::string> stopSeqs;
        std::stringstream ss(g_OllamaStop);
        std::string item;
        while (std::getline(ss, item, ',')) {
            // trim whitespace
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
                stopSeqs.push_back(item.substr(start, end - start + 1));
        }
        if (!stopSeqs.empty())
            requestData["stop"] = stopSeqs;
    }
    if (!g_OllamaSystemPrompt.empty())   requestData["system"] = g_OllamaSystemPrompt;
    if (!g_OllamaSeed.empty())           requestData["seed"] = g_OllamaSeed;

    if (g_ThinkModeEnableForModule)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] LLM set to Think mode.");
        }
        requestData["think"] = true;
        requestData["hidethinking"] = true;
    }

    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading",
                "[Ollama Chat] Failed to reach Ollama AI. cURL error: {}",
                curl_easy_strerror(res));
        }
        return "Failed to reach Ollama AI.";
    }

    std::stringstream ss(responseBuffer);
    std::string line;
    std::ostringstream extractedResponse;

    try
    {
        while (std::getline(ss, line))
        {
            if (line.empty() || std::all_of(line.begin(), line.end(), isspace))
                continue;

            nlohmann::json jsonResponse = nlohmann::json::parse(line);

            if (jsonResponse.contains("response") && !jsonResponse["response"].get<std::string>().empty())
            {
                extractedResponse << jsonResponse["response"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading",
                "[Ollama Chat] JSON Parsing Error: {}",
                e.what());
        }
        return "Error processing response.";
    }

    std::string botReply = extractedResponse.str();

    botReply = ExtractTextBetweenDoubleQuotes(botReply);

    if (botReply.empty())
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] No valid response extracted.");
        }
        return "I'm having trouble understanding.";
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Parsed bot response: {}", botReply);

        if (g_ThinkModeEnableForModule)
        {
            if (g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Bot used think.");
            }
        }
    }

    return botReply;
}

// Function to perform the OpenAI API call.
std::string QueryOpenAIAPI(const std::string& prompt)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Failed to initialize cURL for OpenAI.");
        }
        return "Hmm... I'm lost in thought (OpenAI).";
    }

    std::string url = g_OpenAIEndpointUrl;
    std::string model = g_OpenAIModel;
    std::string apiKey = g_OpenAIApiKey;

    nlohmann::json messages = nlohmann::json::array();
    if (!g_OllamaSystemPrompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", g_OllamaSystemPrompt}});
    }
    messages.push_back({{"role", "user"}, {"content", prompt}});

    nlohmann::json requestData = {
        {"model", model},
        {"messages", messages}
    };

    if (g_OpenAIMaxTokens > 0) requestData["max_tokens"] = g_OpenAIMaxTokens;
    if (g_OpenAITemperature != 0.8f) requestData["temperature"] = g_OpenAITemperature;
    if (g_OpenAITopP != 0.95f) requestData["top_p"] = g_OpenAITopP;
    if (!g_OllamaStop.empty()) {
        // If comma-separated, convert to array
        std::vector<std::string> stopSeqs;
        std::stringstream ss(g_OllamaStop);
        std::string item;
        while (std::getline(ss, item, ',')) {
            // trim whitespace
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
                stopSeqs.push_back(item.substr(start, end - start + 1));
        }
        if (!stopSeqs.empty())
            requestData["stop"] = stopSeqs;
    }
    if (!g_OllamaSeed.empty()) requestData["seed"] = g_OllamaSeed;

    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, fmt::format("Authorization: Bearer {}", apiKey).c_str());

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading",
                "[Ollama Chat] Failed to reach OpenAI API. cURL error: {}",
                curl_easy_strerror(res));
        }
        return "Failed to reach OpenAI API.";
    }

    try
    {
        nlohmann::json jsonResponse = nlohmann::json::parse(responseBuffer);
        if (jsonResponse.contains("choices") && !jsonResponse["choices"].empty())
        {
            if (jsonResponse["choices"][0].contains("message") && jsonResponse["choices"][0]["message"].contains("content"))
            {
                std::string botReply = jsonResponse["choices"][0]["message"]["content"].get<std::string>();
                if (g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Parsed OpenAI response: {}", botReply);
                }
                return botReply;
            }
        }
    }
    catch (const std::exception& e)
    {
        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading",
                "[Ollama Chat] OpenAI JSON Parsing Error: {}",
                e.what());
        }
        return "Error processing OpenAI response.";
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] No valid OpenAI response extracted.");
    }
    return "I'm having trouble understanding (OpenAI).";
}

QueryManager g_queryManager;

// Interface function to submit a query.
std::future<std::string> SubmitQuery(const std::string& prompt)
{
    if (g_LLMProvider == "openai")
    {
        return g_queryManager.submitQuery([prompt]() { return QueryOpenAIAPI(prompt); });
    }
    else // Default to ollama
    {
        return g_queryManager.submitQuery([prompt]() { return QueryOllamaAPI(prompt); });
    }
}
