#ifndef MOD_OLLAMA_CHAT_RAG_H#ifndef MOD_OLLAMA_CHAT_RAG_H

#define MOD_OLLAMA_CHAT_RAG_H#define MOD_OLLAMA_CHAT_RAG_H



#include <string>#include <string>

#include <vector>#include <vector>

#include <unordered_map>#include <unordered_map>

#include <nlohmann/json.hpp>#include <nlohmann/json.hpp>



struct RAGEntry {struct RAGEntry {

    std::string id;    std::string id;

    std::string title;    std::string title;

    std::string content;    std::string content;

    std::vector<std::string> keywords;    std::vector<std::string> keywords;

    std::vector<std::string> tags;    std::vector<std::string> tags;

};};



struct RAGResult {struct RAGResult {

    const RAGEntry* entry;    const RAGEntry* entry;

    float similarity;    float similarity;

};};



class OllamaRAGSystem {class OllamaRAGSystem {

public:public:

    OllamaRAGSystem();    OllamaRAGSystem();

    ~OllamaRAGSystem();    ~OllamaRAGSystem();



    // Initialize the RAG system by loading JSON data files    // Initialize the RAG system by loading JSON data files

    bool Initialize();    bool Initialize();



    // Retrieve relevant information based on a query    // Retrieve relevant information based on a query

    std::vector<RAGResult> RetrieveRelevantInfo(const std::string& query, uint32_t maxResults = 3, float similarityThreshold = 0.3f);    std::vector<RAGResult> RetrieveRelevantInfo(const std::string& query, uint32_t maxResults = 3, float similarityThreshold = 0.3f);



    // Get formatted RAG information for prompt inclusion    // Get formatted RAG information for prompt inclusion

    std::string GetFormattedRAGInfo(const std::vector<RAGResult>& results);    std::string GetFormattedRAGInfo(const std::vector<RAGResult>& results);



private:private:

    // Load RAG data from JSON files in the specified directory    // Load RAG data from JSON files in the specified directory

    bool LoadRAGDataFromDirectory(const std::string& directoryPath);    bool LoadRAGDataFromDirectory(const std::string& directoryPath);



    // Load a single JSON file    // Load a single JSON file

    bool LoadRAGDataFromFile(const std::string& filePath);    bool LoadRAGDataFromFile(const std::string& filePath);



    // Calculate similarity between query and entry    // Calculate similarity between query and entry

    float CalculateSimilarity(const std::string& query, const RAGEntry& entry);    float CalculateSimilarity(const std::string& query, const RAGEntry& entry);



    // Simple text preprocessing (lowercase, remove punctuation)    // Simple text preprocessing (lowercase, remove punctuation)

    std::string PreprocessText(const std::string& text) const;    std::string PreprocessText(const std::string& text) const;



    // Split text into words    // Split text into words

    std::vector<std::string> TokenizeText(const std::string& text) const;    std::vector<std::string> TokenizeText(const std::string& text) const;



    // Calculate cosine similarity between two vectors    // Calculate cosine similarity between two vectors

    float CalculateCosineSimilarity(const std::vector<float>& vec1, const std::vector<float>& vec2) const;    float CalculateCosineSimilarity(const std::vector<float>& vec1, const std::vector<float>& vec2) const;



    // Convert text to simple TF vector (term frequency)    // Convert text to simple TF vector (term frequency)

    std::vector<float> TextToTFVector(const std::string& text, const std::vector<std::string>& vocabulary) const;    std::vector<float> TextToTFVector(const std::string& text, const std::vector<std::string>& vocabulary) const;



private:private:

    std::vector<RAGEntry> m_ragEntries;    std::vector<RAGEntry> m_ragEntries;

    std::vector<std::string> m_vocabulary;    std::vector<std::string> m_vocabulary;

    bool m_initialized;    bool m_initialized;

};};



#endif // MOD_OLLAMA_CHAT_RAG_H#endif // MOD_OLLAMA_CHAT_RAG_H