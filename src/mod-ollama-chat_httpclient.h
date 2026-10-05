#ifndef MOD_OLLAMA_CHAT_HTTPCLIENT_H
#define MOD_OLLAMA_CHAT_HTTPCLIENT_H

#include <string>
#include <utility>
#include <vector>

// Extra request headers, e.g. the Authorization / x-api-key a hosted provider
// needs. Carried per call so the value comes from the caller's config
// snapshot rather than from a string global a worker must not read.
using OllamaHttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Full result of an HTTP call, so callers can distinguish "model refused" from
// "server unreachable" -- the old client returned an empty string for both,
// which is why an unsupported think mode looked identical to a dead Ollama.
struct OllamaHttpResult
{
    int         status = 0;      // HTTP status, 0 when the request never completed
    std::string body;
    std::string error;           // transport-level error text, empty on success
    int         retryAfterSeconds = 0;   // from a Retry-After header, 0 when absent

    bool ok() const { return status == 200; }
};

// True when the module was compiled with OpenSSL, i.e. https:// URLs work.
bool OllamaHttp_TlsAvailable();

class OllamaHttpClient
{
public:
    OllamaHttpClient();
    ~OllamaHttpClient();

    // Backwards-compatible form: body on 200, empty string otherwise.
    std::string Post(const std::string& url, const std::string& jsonData);

    // Preferred form. Connections are pooled per worker thread, so repeated
    // calls reuse a keep-alive socket without serialising concurrent workers
    // behind a single shared client.
    // timeoutOverride > 0 uses that instead of the configured timeout; the
    // capability probe wants to fail fast rather than hang startup diagnostics.
    // verifyCerts turns on TLS server certificate verification (system trust
    // store). Off by default for backwards compatibility with self-signed
    // reverse proxies; the API layer turns it on when an API key is in play.
    OllamaHttpResult PostEx(const std::string& url, const std::string& jsonData,
                            int timeoutOverride = 0,
                            const OllamaHttpHeaders& extraHeaders = {},
                            bool verifyCerts = false);
    OllamaHttpResult GetEx(const std::string& url, int timeoutOverride = 0);

    void SetTimeout(int seconds);
    bool IsAvailable() const;

private:
    int  m_timeout;
    bool m_available;
};

// Strip the API path off a configured endpoint so sibling endpoints can be
// derived: "http://host:11434/api/generate" -> "http://host:11434"
std::string OllamaDeriveBaseUrl(const std::string& url);

#endif // MOD_OLLAMA_CHAT_HTTPCLIENT_H
