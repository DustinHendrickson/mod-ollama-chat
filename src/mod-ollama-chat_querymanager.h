#ifndef MOD_OLLAMA_CHAT_QUERYMANAGER_H
#define MOD_OLLAMA_CHAT_QUERYMANAGER_H

#include <string>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <functional> // Add this include for std::function


class QueryManager {
public:
    QueryManager();
    void setMaxConcurrentQueries(int maxQueries);
    std::future<std::string> submitQuery(std::function<std::string()> apiCall);

private:
    struct QueryTask {
        std::function<std::string()> apiCall;
        std::promise<std::string> promise;
    };

    void processQuery(std::function<std::string()> apiCall, std::promise<std::string> promise);

    int maxConcurrentQueries; // 0 means no limit
    int currentQueries;
    std::mutex mutex_;
    std::queue<QueryTask> taskQueue;
};

#endif // MOD_OLLAMA_CHAT_QUERYMANAGER_H
