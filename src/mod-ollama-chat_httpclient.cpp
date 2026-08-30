#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat_config.h"

#include <httplib.h>

#include "Log.h"
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace
{
    struct ParsedUrl
    {
        bool        valid = false;
        bool        https = false;
        std::string host;
        int         port = 0;
        std::string path;
    };

    ParsedUrl ParseUrl(const std::string& url)
    {
        ParsedUrl out;

        static const std::regex urlRegex(R"(^(https?)://([^:/?#]+)(?::(\d+))?([/?#].*)?$)");
        std::smatch match;
        if (!std::regex_match(url, match, urlRegex))
            return out;

        const std::string protocol = match[1].str();
        out.https = (protocol == "https");
        out.host  = match[2].str();

        if (match[3].matched)
        {
            out.port = std::stoi(match[3].str());
        }
        else
        {
            // No explicit port. Use the scheme default -- NOT Ollama's 11434,
            // which silently misrouted every reverse-proxied deployment on :80.
            out.port = out.https ? 443 : 80;
        }

        out.path  = match[4].matched ? match[4].str() : "/";
        out.valid = true;
        return out;
    }

    httplib::Headers BuildHeaders(const std::string& host)
    {
        httplib::Headers headers = {
            { "Content-Type", "application/json" },
            { "User-Agent",   "AzerothCore-OllamaChat/2.0" },
            { "Accept",       "application/json" },
        };

        if (host.find("ngrok") != std::string::npos)
            headers.emplace("ngrok-skip-browser-warning", "true");

        return headers;
    }

    // One connection per worker thread per endpoint. A single shared client
    // would keep-alive correctly but cpp-httplib serialises requests on it,
    // which would collapse MaxConcurrentQueries back down to 1.
    struct ClientCache
    {
        std::unordered_map<std::string, std::unique_ptr<httplib::Client>> plain;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        std::unordered_map<std::string, std::unique_ptr<httplib::SSLClient>> ssl;
#endif
        int timeout = -1;
    };

    thread_local ClientCache t_clients;

    void ApplyTimeout(httplib::Client& c, int timeout)
    {
        c.set_connection_timeout(timeout);
        c.set_read_timeout(timeout);
        c.set_write_timeout(timeout);
        c.set_keep_alive(true);
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    void ApplyTimeout(httplib::SSLClient& c, int timeout)
    {
        c.set_connection_timeout(timeout);
        c.set_read_timeout(timeout);
        c.set_write_timeout(timeout);
        c.set_keep_alive(true);
    }
#endif

    // Drop pooled sockets when the configured timeout changes.
    void InvalidateIfTimeoutChanged(int timeout)
    {
        if (t_clients.timeout == timeout)
            return;
        t_clients.plain.clear();
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        t_clients.ssl.clear();
#endif
        t_clients.timeout = timeout;
    }

    OllamaHttpResult Perform(const ParsedUrl& u, int timeout,
                             const std::string& jsonData, bool isPost)
    {
        OllamaHttpResult result;
        const std::string key = u.host + ":" + std::to_string(u.port);

        InvalidateIfTimeoutChanged(timeout);

        httplib::Result response(nullptr, httplib::Error::Unknown);
        const httplib::Headers headers = BuildHeaders(u.host);

        if (u.https)
        {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            auto& slot = t_clients.ssl[key];
            if (!slot)
            {
                slot = std::make_unique<httplib::SSLClient>(u.host, u.port);
                slot->enable_server_certificate_verification(false);
                ApplyTimeout(*slot, timeout);
            }
            response = isPost ? slot->Post(u.path, headers, jsonData, "application/json")
                              : slot->Get(u.path, headers);
#else
            result.error = "HTTPS requested but the module was built without OpenSSL support.";
            return result;
#endif
        }
        else
        {
            auto& slot = t_clients.plain[key];
            if (!slot)
            {
                slot = std::make_unique<httplib::Client>(u.host, u.port);
                ApplyTimeout(*slot, timeout);
            }
            response = isPost ? slot->Post(u.path, headers, jsonData, "application/json")
                              : slot->Get(u.path, headers);
        }

        if (!response)
        {
            result.error = httplib::to_string(response.error());

            // A dead keep-alive socket surfaces as a transport error; drop the
            // cached client so the next attempt reconnects cleanly.
            if (u.https)
            {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                t_clients.ssl.erase(key);
#endif
            }
            else
            {
                t_clients.plain.erase(key);
            }
            return result;
        }

        result.status = response->status;
        result.body   = response->body;
        return result;
    }
}

// --------------------------------------------------------------------------

std::string OllamaDeriveBaseUrl(const std::string& url)
{
    ParsedUrl u = ParseUrl(url);
    if (!u.valid)
        return url;

    std::ostringstream oss;
    oss << (u.https ? "https://" : "http://") << u.host;

    const int defaultPort = u.https ? 443 : 80;
    if (u.port != defaultPort)
        oss << ":" << u.port;

    return oss.str();
}

OllamaHttpClient::OllamaHttpClient()
    : m_timeout(120), m_available(true)
{
}

OllamaHttpClient::~OllamaHttpClient() = default;

void OllamaHttpClient::SetTimeout(int seconds)
{
    m_timeout = seconds;
}

bool OllamaHttpClient::IsAvailable() const
{
    return m_available;
}

OllamaHttpResult OllamaHttpClient::PostEx(const std::string& url, const std::string& jsonData,
                                          int timeoutOverride)
{
    OllamaHttpResult result;

    try
    {
        ParsedUrl u = ParseUrl(url);
        if (!u.valid)
        {
            result.error = "Invalid URL format: " + url;
            LOG_ERROR("module.ollamachat", "[Ollama Chat] {}", result.error);
            return result;
        }

        const int timeout = timeoutOverride > 0
                                ? timeoutOverride
                                : (g_HttpTimeoutSeconds > 0
                                       ? static_cast<int>(g_HttpTimeoutSeconds)
                                       : m_timeout);

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] POST {}:{}{}", u.host, u.port, u.path);

        result = Perform(u, timeout, jsonData, true);

        if (!result.error.empty())
            LOG_ERROR("module.ollamachat", "[Ollama Chat] HTTP POST to {}:{}{} failed: {}",
                      u.host, u.port, u.path, result.error);
        else if (!result.ok() && g_DebugEnabled)
            LOG_INFO("module.ollamachat", "[Ollama Chat] HTTP {} from {}{} body: {}",
                     result.status, u.host, u.path, result.body);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
        LOG_ERROR("module.ollamachat", "[Ollama Chat] HTTP client exception: {}", e.what());
    }

    return result;
}

OllamaHttpResult OllamaHttpClient::GetEx(const std::string& url, int timeoutOverride)
{
    OllamaHttpResult result;

    try
    {
        ParsedUrl u = ParseUrl(url);
        if (!u.valid)
        {
            result.error = "Invalid URL format: " + url;
            return result;
        }

        const int timeout = timeoutOverride > 0
                                ? timeoutOverride
                                : (g_HttpTimeoutSeconds > 0
                                       ? static_cast<int>(g_HttpTimeoutSeconds)
                                       : m_timeout);

        result = Perform(u, timeout, std::string(), false);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
    }

    return result;
}

std::string OllamaHttpClient::Post(const std::string& url, const std::string& jsonData)
{
    OllamaHttpResult r = PostEx(url, jsonData);
    return r.ok() ? r.body : std::string();
}
